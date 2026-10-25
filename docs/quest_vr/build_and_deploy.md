# Build and deploy (Quest 3)

## Toolchain (owner's Windows machine)

- Android SDK at `%LOCALAPPDATA%\Android\Sdk` (`ANDROID_HOME` set, so no `local.properties`).
- **NDK 30.0.15729638** is required by `android/app/build.gradle.kts`; installed with
  `sdkmanager "ndk;30.0.15729638"`. (BattleShip pins NDK 29 instead.)
- CMake 3.31.6 from the SDK; Java 17; Gradle 9.4 via the wrapper.
- No `gh` CLI. Git pushes over HTTPS work with the stored credentials.

## Build

```bash
cd android && ./gradlew.bat --no-daemon assembleDebug
# → android/app/build/outputs/apk/debug/app-debug.apk (~42 MB, arm64 only)
```

- First build ~9 min; touching one libultraship file ~30 s; a change that alters
  compile flags for every TU ~5–9 min.
- Native code is always built `-DCMAKE_BUILD_TYPE=Release` (the game needs
  `-fno-strict-aliasing`, which only the Release flags carry). CMake tree:
  `android/app/.cxx/Release/<hash>/arm64-v8a/` (has `compile_commands.json`).
- Long builds: run in the background and redirect output to a log in the
  scratchpad; grep for `BUILD S|BUILD F| error`.

## Install and launch

```bash
adb connect <headset-ip>:5555          # wireless adb; accept prompt in headset
adb -s <headset-ip>:5555 install -r '<repo>\android\app\build\outputs\apk\debug\app-debug.apk'
MSYS_NO_PATHCONV=1 adb -s <headset-ip>:5555 shell am start -n dev.net64.paperboat.debug/dev.net64.paperboat.LauncherActivity
```

- Package `dev.net64.paperboat.debug`. `LauncherActivity` (2D, own process
  `:launcher`) unpacks bundled data, takes the ROM, runs Torch extraction, then
  starts `MainActivity` (the SDL game).
- After a headset reboot: USB, `adb tcpip 5555`, reconnect, accept the prompt.
- The headset must be worn for the game to render/log meaningfully.

## ROM

- ROM: `Paper Mario (USA).z64`, SHA1
  `3837f44cda784b466c9a2d99df70d77c322b97a0` (the supported US dump).
- A copy is on the headset at `/sdcard/Download/Paper Mario (USA).z64`; select it
  with the launcher's file picker (in the headset).
- **Pushing the ROM into the app's folder with adb does not work**: files created
  by the `shell` user in `Android/data/<pkg>/files` are not readable by the app,
  and `run-as` can't write shared storage. Use the picker.
- Extraction result: `pm64.o2r` (~40 MB) in the app folder. It survives
  reinstalls (`install -r`) — only uninstall / clear data removes it.

## App files and logs

`/sdcard/Android/data/dev.net64.paperboat.debug/files/`:
`pm64.o2r`, `paperboat.o2r`, `paperboat.cfg.json` (CVars), `logs/Paperboat.log`,
`baserom.us.z64`, `config.yml`, `assets/`, `mods/`.

- `Paperboat.log` is **appended across launches** — look after the last
  `Initializing audio system...` line for the current run.
- Read app-owned files with `adb exec-out cat <path>`; `adb pull` fails on names
  containing `?`/`:` (Windows), use `adb exec-out "cd <dir> && tar cf - <files>" > x.tar`.
- adb (`shell` user) can't delete or create files the app owns, and the app can't
  write into folders created by adb. Diagnostics that write files should write
  into an app-created folder (e.g. `files/logs/`).
- `adb logcat -s Paperboat` shows the launcher/extraction messages.

## Screenshots

`adb exec-out screencap -p > shot.png` captures the compositor view (both eyes,
4128×2208, fisheye, passthrough included). Crop the left half and downscale to
inspect. It only shows the game if the owner is looking at the panel.

## Shell gotchas (Git Bash on Windows)

- Prefix adb commands that take `/sdcard/...` paths with `MSYS_NO_PATHCONV=1`,
  or Git Bash rewrites them into Windows paths.
- Pass the APK path as a quoted Windows path (`'<repo>\...\app-debug.apk'`) to
  `adb install`; the `/c/...` form fails.
- Foreground `sleep` loops are blocked by the harness; wait with
  `until <condition>; do sleep N; done` or `run_in_background`.
