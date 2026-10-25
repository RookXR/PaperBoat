package dev.net64.paperboat

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity

/**
 * The game. The on-screen controller is the engine's own, drawn inside the GL
 * surface (src/port/ui/TouchControls.cpp); the only thing layered over SDL is a
 * Mods button, shown while the engine's menu is up.
 *
 * [LauncherActivity] guarantees pm64.o2r exists before this activity starts.
 */
class MainActivity : SDLActivity() {

    private lateinit var modsButton: Button

    private var menuOpen = false

    private val handler = Handler(Looper.getMainLooper())
    private val menuWatcher = object : Runnable {
        override fun run() {
            syncMenuState()
            handler.postDelayed(this, MENU_POLL_MS)
        }
    }

    // org/libsdl/app stays byte-identical to the SDL release libultraship
    // pins — SDLActivity refuses to start if the two disagree on version — so
    // the library is named here rather than patched in there.
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "main")

    private external fun isMenuOpen(): Boolean

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        goFullscreen()
        // The Quest build runs immersive, where nothing on the Android surface
        // is visible or touchable.
        if (!BuildConfig.QUEST_XR) {
            addModsButton()
        }
    }

    /**
     * Quest: pin SDL's surface to the size the game renders at in VR. The
     * immersive activity's surface otherwise spans both eye panels (~1.87:1),
     * the wrong aspect and far more pixels than the virtual screen shows.
     * surfaceChanged then reports the fixed size to SDL as a window resize.
     * Called from native code on the SDL thread (src/port/xr/quest_xr.cpp).
     */
    @Suppress("unused")
    fun setXrRenderSize(width: Int, height: Int) {
        runOnUiThread { mSurface?.holder?.setFixedSize(width, height) }
    }

    /**
     * Edge to edge, bars hidden. From API 35 the old fullscreen window flags are
     * ignored, so the bars go through the insets controller instead.
     * Transient-by-swipe keeps a stray swipe from resizing the window and
     * forcing the engine to rebuild its framebuffers.
     */
    private fun goFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)

        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        // Render into the cutout too, rather than letterboxing beside it.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }
    }

    /**
     * Quest: in an immersive activity the 2D window never gains focus, and
     * SDLActivity pauses the native thread on focus loss — which would freeze
     * the game before its first frame. The OpenXR session state gates input
     * instead.
     */
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(BuildConfig.QUEST_XR || hasFocus)
        // The bars return on any focus loss; put them away again coming back.
        if (hasFocus) {
            goFullscreen()
        }
    }

    override fun onResume() {
        super.onResume()
        // Also re-sync now, so returning here never shows a stale menu state.
        syncMenuState()
        handler.removeCallbacks(menuWatcher)
        handler.postDelayed(menuWatcher, MENU_POLL_MS)
    }

    override fun onPause() {
        handler.removeCallbacks(menuWatcher)
        super.onPause()
    }

    override fun onDestroy() {
        handler.removeCallbacks(menuWatcher)
        val finishing = isFinishing && !isChangingConfigurations
        super.onDestroy()
        // Quest: the OpenXR instance/session and the virtual Touch gamepad are
        // process-lifetime statics tied to this SDL run. End the process once
        // the game has shut down so a relaunch starts from a clean slate.
        if (BuildConfig.QUEST_XR && finishing) {
            android.os.Process.killProcess(android.os.Process.myPid())
        }
    }

    /**
     * The menu can be opened by its own button, a keyboard or a gamepad, and can
     * close itself, so ask the engine rather than track it here.
     */
    private fun syncMenuState() {
        if (!::modsButton.isInitialized) return
        val open = runCatching { isMenuOpen() }.getOrDefault(false)
        if (open == menuOpen) return

        menuOpen = open
        modsButton.visibility = if (open) android.view.View.VISIBLE else android.view.View.GONE
    }

    /** Bottom-left, clear of the engine's menu toggle and of the menu itself. */
    private fun addModsButton() {
        val margin = (16 * resources.displayMetrics.density).toInt()

        modsButton = Button(this).apply {
            text = getString(R.string.mods_button)
            textSize = 12f
            isAllCaps = false
            visibility = android.view.View.GONE
            setOnClickListener { startActivity(Intent(context, ModsActivity::class.java)) }
        }

        val layout = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM or Gravity.START
        ).apply { setMargins(margin, margin, margin, margin) }

        findViewById<ViewGroup>(android.R.id.content).addView(modsButton, layout)
    }

    private companion object {
        /** Fast enough to feel immediate on a menu toggle, cheap enough to ignore. */
        const val MENU_POLL_MS = 150L
    }
}
