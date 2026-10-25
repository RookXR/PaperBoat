import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.security.MessageDigest
import java.util.Properties
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val repositoryRoot: File = rootProject.projectDir.parentFile

// One source of truth for the version: the CMake project declaration.
val projectVersion: String =
    Regex("""project\s*\([^)]*VERSION\s+([0-9.]+)""")
        .find(File(repositoryRoot, "CMakeLists.txt").readText())
        ?.groupValues?.get(1)
        ?: "0.0.0"

// Release signing: android/key.properties locally, the environment on CI. The
// file holds passwords, so it is gitignored.
val keystoreProperties = Properties().apply {
    val file = rootProject.file("key.properties")
    if (file.isFile) file.inputStream().use { load(it) }
}

// With neither configured, the release build falls back to the debug key so a
// fresh clone still gets an installable APK.
val hasReleaseKeystore =
    keystoreProperties.getProperty("storeFile") != null || System.getenv("KEYSTORE_FILE") != null

// Meta Quest VR build: -Ppaperboat.quest=true. Runs the game activity as an
// immersive OpenXR app (stereo screen + Touch controller input,
// src/port/xr/quest_xr.cpp). Off by default, so phone/tablet APKs are
// unchanged. Same applicationId either way, so a Quest that already extracted
// the ROM with the 2D build keeps its pm64.o2r.
val paperboatQuest: Boolean =
    (project.findProperty("paperboat.quest") as String?)?.toBoolean() ?: false

/**
 * Everything the device needs that is not the ROM, in one zip inside the APK:
 * Torch's recipes (config.yml + assets/) and the engine's own paperboat.o2r.
 *
 * The o2r is built here from `port/` rather than taken from a desktop build,
 * whose POST_BUILD copy would land inside the NDK build tree and never reach
 * the APK. The launcher unpacks the zip on first run (see GameAssets.kt) and
 * compares the digest beside it to decide whether to unpack again.
 */
abstract class PackGameData : DefaultTask() {

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val recipes: DirectoryProperty

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val portData: DirectoryProperty

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val lusShaders: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val config: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun pack() {
        val recipesRoot = recipes.get().asFile
        val portRoot = portData.get().asFile

        val target = outputDir.get().asFile
        target.mkdirs()

        // Built first, then added whole, so the bundled archive is byte-for-byte
        // the file the game opens.
        val engine = File(target, ENGINE_ARCHIVE)
        zipTree(mapOf("" to portRoot, "shaders" to lusShaders.get().asFile), engine)

        val entries = buildList {
            add("config.yml" to config.get().asFile)
            add(ENGINE_ARCHIVE to engine)
            recipesRoot.walkTopDown().filter(File::isFile).forEach {
                add("assets/${it.relativeTo(recipesRoot).invariantSeparatorsPath}" to it)
            }
        }.sortedBy { it.first }

        // Sorted, fixed timestamp: a stable digest means an update only
        // re-unpacks when the contents actually changed.
        val bundle = File(target, "gamedata.zip")
        ZipOutputStream(bundle.outputStream().buffered()).use { zip ->
            entries.forEach { (name, file) ->
                zip.putNextEntry(ZipEntry(name).apply { time = FIXED_ENTRY_TIME })
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
            }
        }
        engine.delete()

        val digest = MessageDigest.getInstance("MD5").digest(bundle.readBytes())
        File(target, "gamedata.version").writeText(digest.joinToString("") { "%02x".format(it) })
    }

    private fun zipTree(roots: Map<String, File>, destination: File) {
        val entries = roots.flatMap { (prefix, root) ->
            root.walkTopDown().filter(File::isFile).map { file ->
                val relative = file.relativeTo(root).invariantSeparatorsPath
                (if (prefix.isEmpty()) relative else "$prefix/$relative") to file
            }
        }.sortedBy { it.first }

        ZipOutputStream(destination.outputStream().buffered()).use { zip ->
            entries.forEach { (name, file) ->
                zip.putNextEntry(ZipEntry(name).apply { time = FIXED_ENTRY_TIME })
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
            }
        }
    }

    private companion object {
        const val ENGINE_ARCHIVE = "paperboat.o2r"

        // 1980-02-01T00:00:00Z, as Gradle's own archive tasks use. Earlier
        // risks falling below the 1980 floor of a zip entry's DOS timestamp.
        const val FIXED_ENTRY_TIME = 318211200000L
    }
}

// Via the variant API, not a source-set srcDir: srcDir takes the path but not
// the dependency on the task that fills it, so a clean checkout would silently
// package an APK with none of this in it.
androidComponents {
    onVariants { variant ->
        val packGameData =
            tasks.register<PackGameData>("pack${variant.name.replaceFirstChar(Char::titlecase)}GameData") {
                description = "Bundles the Torch recipes and the engine archive into the APK."
                recipes.set(File(repositoryRoot, "assets"))
                portData.set(File(repositoryRoot, "port"))
                lusShaders.set(File(repositoryRoot, "external/libultraship/src/fast/shaders"))
                config.set(File(repositoryRoot, "config.yml"))
            }
        variant.sources.assets?.addGeneratedSourceDirectory(packGameData, PackGameData::outputDir)
    }
}

android {
    namespace = "dev.net64.paperboat"
    compileSdk = 36
    ndkVersion = "30.0.15729638"

    defaultConfig {
        applicationId = "dev.net64.paperboat"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = projectVersion

        buildConfigField("boolean", "QUEST_XR", paperboatQuest.toString())
        // Category on MainActivity's MAIN intent-filter. Horizon OS runs an
        // activity carrying IMMERSIVE_HMD in VR instead of a 2D panel; on other
        // builds the filter is an inert MAIN/DEFAULT pair.
        manifestPlaceholders["gameActivityCategory"] =
            if (paperboatQuest) "org.khronos.openxr.intent.category.IMMERSIVE_HMD"
            else "android.intent.category.DEFAULT"

        ndk {
            // 64-bit only; a second ABI roughly doubles an already long
            // native build. Add "armeabi-v7a" if you need 32-bit devices.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DUSE_OPENGLES=ON",
                    "-DSDL_SHARED=ON",
                    "-DSDL_STATIC=OFF",
                    "-DHAVE_LD_VERSION_SCRIPT=OFF",
                    // Every variant, debug included. Only the Release flags
                    // carry -fno-strict-aliasing, and the game's C needs it:
                    // at -O2 aliasing miscompiles it into graphical glitches.
                    // The plugin otherwise builds release as RelWithDebInfo.
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DPAPERBOAT_QUEST_XR=${if (paperboatQuest) "ON" else "OFF"}"
                )
                targets += "Paperboat"
            }
        }
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                val configuredStore = keystoreProperties.getProperty("storeFile")
                if (configuredStore != null) {
                    // Relative to key.properties' own directory.
                    storeFile = rootProject.file(configuredStore)
                    storePassword = keystoreProperties.getProperty("storePassword")
                    keyAlias = keystoreProperties.getProperty("keyAlias")
                    keyPassword = keystoreProperties.getProperty("keyPassword")
                } else {
                    storeFile = file(System.getenv("KEYSTORE_FILE"))
                    storePassword = System.getenv("KEYSTORE_PASSWORD")
                    keyAlias = System.getenv("KEY_ALIAS")
                    keyPassword = System.getenv("KEY_PASSWORD")
                }
            }
        }
    }

    buildTypes {
        debug {
            // No isJniDebuggable: the native side is Release regardless.
            applicationIdSuffix = ".debug"
        }
        release {
            // Shrinking the tiny Kotlin layer buys nothing and risks stripping
            // the classes the JNI bridge looks up by name.
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (hasReleaseKeystore) "release" else "debug")
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = File(repositoryRoot, "CMakeLists.txt")
            // libultraship needs CMake >= 3.24; the NDK-bundled 3.22.1 is too old.
            version = "3.30.3+"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        abortOnError = false
    }

    buildFeatures {
        buildConfig = true
        // Quest: consume the OpenXR loader AAR's prefab package from CMake.
        prefab = paperboatQuest
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-ktx:1.9.3")

    // Khronos OpenXR loader (prefab: OpenXR::openxr_loader). Quest build only.
    if (paperboatQuest) {
        implementation("org.khronos.openxr:openxr_loader_for_android:1.1.60")
    }
}
