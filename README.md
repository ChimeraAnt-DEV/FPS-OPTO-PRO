# FPS-OPTO-PRO

A native FPS mod for ChimeraLauncher that removes render-path overhead in
Minecraft Bedrock without changing a single pixel.

The target case is a weak device running MTBinloader2 with a heavy realistic
shader, where the frame budget is already spent before the CPU gets to draw
anything. The mod does not make the shader cheaper; it stops the renderer
paying for work whose result the driver discards.

## What it does

Every lever is timing-only. Nothing here changes what is drawn, when a frame is
presented, or the resolution.

1. **Redundant GL ES call filtering.** The game re-sets state the driver already
   holds (blend modes, depth/cull/stencil state, viewport, buffer and texture
   bindings). Those calls are recognised against a shadow copy of the context
   state and dropped before they reach the driver. On a weak GPU the driver's
   validation work per call is a real fraction of the frame.

2. **Read-only frame sensor.** Frame interval and GPU workload are measured at
   the swap boundary. The swap is always forwarded unchanged.

3. **Frame-aware scheduling.** Render threads get an advisory priority lift
   based on whether the frame is CPU-bound or GPU-bound. Under a GPU bound the
   lift is deliberately pulled back to zero, because raising priority there
   only parks the render thread on a core the driver's submission thread wants.
   Fully reversible: the original priority is recorded before anything changes.

4. **Optional ADPF hint session** (off by default). Where the platform offers
   `APerformanceHint`, the mod reports actual frame times and asks for
   efficiency-leaning operating points, which is what protects the *sustained*
   frame rate on a thermally limited device.

## Why it is invisible

The correctness rules that make this safe:

* A field whose default the GL ES spec does not pin down starts at an
  impossible sentinel, so the first call of that kind is always forwarded and
  the shadow is learned from the real stream rather than guessed.
* The only capability the spec starts *enabled* is `GL_DITHER`; the shadow
  starts from exactly that, so a caller's `glEnable(GL_CULL_FACE)` is never
  dropped.
* Texture bindings are tracked per texture unit. A stale entry would make a
  pass sample the wrong image, so any target that cannot be addressed is always
  forwarded.
* The shadow resets on a context change and only then, since state belongs to
  the context and survives being made un-current.
* Nothing touches swap interval, frame pacing, or the drawable.

## Install

Copy the mod into the launcher's mods directory:

```
mods/FPS-OPTO-PRO/
  manifest.json
  libfps_opto_pro.so
```

The launcher picks it up from `manifest.json` (`"type": "preload-native"`,
`"entry": "libfps_opto_pro.so"`). arm64-v8a only, matching the launcher's
shipped ABI.

## Configuration

Options are exposed in the launcher's Mod Menu, and persisted through the
preloader config file. Defaults are the safe baseline: filtering and scheduling
on, ADPF off.

| Option | Default | Effect |
| --- | --- | --- |
| Skip redundant GL calls | on | Drop GL ES state calls the driver would ignore anyway |
| Frame-aware scheduling | on | Advisory render-thread priority, reversible |
| Workload shape | Auto | Reads the CPU/GPU balance from live telemetry when Auto |
| Target FPS | 0 (auto) | 0 adopts whatever the display prefers |
| Max priority boost | 2 | Upper bound on the nice-value reduction |
| Log frame stats | off | Write measured frame timings to logcat |

Set `logStats` to confirm the effect on a real device: the log line reports
frame time, fps, and the per-frame GL call suppression ratio.

## Build

Host tests need no SDK:

```
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
./build-host/tests/fps_opto_pro_tests
```

Android:

```
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release -DFPS_OPTO_PRO_BUILD_TESTS=OFF \
  -DPRELOADER_ANDROID_ROOT=/path/to/preloader-android
cmake --build build-android
```

Output: `build-android/out/arm64-v8a/libfps_opto_pro.so`. 
