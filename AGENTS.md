# AGENTS.md

Repository-specific knowledge for FPS-OPTO-PRO.

## What this is

Native (`.so`) FPS mod for ChimeraLauncher (Minecraft Bedrock, Android).
Target: weak arm64 device running MTBinloader2 with a heavy realistic shader,
holding 30-60 FPS with **zero** visual change. Every lever is timing-only.

## Layout

- `src/` — mod sources. `FpsOptoPro.cpp` is the lifecycle/ModMenu entry.
- `tests/` — host-only tests (no Android SDK, no NDK needed).
- `manifest.json` — launcher contract: `"type": "preload-native"`, `"entry": "libfps_opto_pro.so"`.

## Build

Host tests:
```
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host
./build-host/tests/fps_opto_pro_tests
```

Android (arm64-v8a only):
```
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release -DFPS_OPTO_PRO_BUILD_TESTS=OFF \
  -DPRELOADER_ANDROID_ROOT=/path/to/preloader-android
cmake --build build-android
```
Output: `build-android/out/arm64-v8a/libfps_opto_pro.so`.

## Critical invariants (do not break)

1. **Hook call-through must use the trampoline.** `pl::memory::hook()` replaces
   the exported symbol, so calling a hooked function *by name* from inside its
   own detour recurses forever. Always call through the stored original:
   `FPSOPTO_ORIGINAL(kEntryX, glX)` / `eglOriginal(kEglX)`. Grep for bare
   hooked-symbol calls as a regression check.
2. **GL ES initial state.** The only capability enabled at context creation is
   `GL_DITHER`. `GL_CULL_FACE` starts **disabled**. Getting this wrong drops a
   real `glEnable` and changes what is drawn.
3. **Unknowable defaults use sentinels.** Values the spec does not pin down
   (viewport, program, object bindings, pixel-store) start at an impossible
   sentinel so the first call is always forwarded.
4. **Texture bindings are per-unit.** Never share across units; forward any
   target that cannot be addressed.
5. **Shadow resets on context change only.**
6. **Never touch swap interval, pacing, or the drawable.**

## ChimeraLauncher / preloader contracts

- Mod dir layout: `mods/<Name>/{manifest.json, libfps_opto_pro.so}`.
- The launcher reads `manifest.json` directly from the mod directory.
- Launcher Java package is `org.chimeramc.launcher`, but the preloader JNI
  symbols use `Java_org_levimc_launcher_...` (launcher rebinds the package).
- **ModMenu Radio contract**: the option list is passed in `minValue`
  (comma-separated labels); the UI reports the **selected index** as the value
  string, not the label. Parse both index and name.
- `ConfigType` numeric order matters for the launcher's type mapping:
  Toggle=0, SliderInt=1, SliderFloat=2, Radio=3, Color=4, Keybind=5, Text=6, Button=7.
- `pl::mod` is an alias of `pl::mod` (`NativeMod`, `ModContext`, ...).
- Signature/module resolution dlopens `RTLD_NOLOAD` then plain `dlopen`; GL/EGL
  hooks only bind if the module is already resident at enable time.