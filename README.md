# LiveSketch

This Android app was developed to be actively used by students during class sessions at my university (unofficial request). While it is still in early development, it is already functional and usable for drawing and testing purposes.

LiveSketch allows users to draw on a multi-layer canvas and stream the result in real time using NDI (Network Device Interface). This makes it suitable for live demos, classroom activities, or experimental creative workflows where remote or shared visuals are needed.

## Features

- Multi-layer canvas: add, delete, rename, move up/down, duplicate, merge down and clear layers, with per-layer visibility and opacity.
- Pressure-sensitive brushes (four brush textures), color, size and stroke opacity. Eraser from the menu or from the stylus eraser tip.
- Pan and pinch-zoom with two fingers; one finger pans, or draws if "Dibujar con el dedo" is enabled. With a mouse: left button draws, right/middle button pans, wheel zooms.
- NDI output of the full canvas (source name `LiveSketch`), independent of the on-screen zoom.
- "Guardar PNG" saves the full canvas to Downloads as `LiveSketch_YYYYMMDD_HHMMSS.png`, with transparency.
- The Android back button asks before exiting.

## Building

Clone with submodules (SDL3 and glm are git submodules; Dear ImGui is vendored):

```sh
git clone --recursive https://github.com/TaCodeC/LiveSketch-Android.git
# or, in an existing clone:
git submodule update --init --recursive
```

### Android

Requirements: Android Studio (or the Android SDK 35 command-line tools), the NDK and CMake 3.22+.

The NDI SDK is not included in the repository. Download the *NDI SDK for Android* and point the build to it in `local.properties` (or with the `NDI_SDK_DIR` environment variable):

```properties
ndi.sdk.dir=/path/to/NDI SDK for Android
```

The folder must contain `include/Processing.NDI.Lib.h` and `lib/<abi>/libndi.so`. Without it the app still builds, with the NDI option disabled.

Then build and run from Android Studio, or:

```sh
./gradlew assembleDebug
```

### Desktop (development and tests)

The same C++ code runs in a desktop window on Linux with OpenGL ES 3.0 (Mesa). It is useful to try changes without a device and to run the automated tests.

```sh
sudo apt install build-essential cmake ninja-build libgles-dev libegl-dev libx11-dev libxext-dev
cmake -S app -B build -G Ninja -DLIVESKETCH_BUILD_TESTS=ON    # add -DNDI_SDK_DIR=... for NDI
cmake --build build
./build/livesketch
(cd build && ctest --output-on-failure)
```

The tests create a real OpenGL ES 3.0 context and also run without a display (SDL's offscreen driver).

## Project layout

```
app/src/main/cpp/
  main.cpp     SDL entry points (main callbacks)
  App/         lifecycle, GL context, frame pacing and input routing
  Canvas/      layers, brush, compositing, camera and canvas view
  Gfx/         OpenGL helpers (RAII objects, shaders, pixel conversion)
  IO/          assets and PNG export
  NDI/         NDI output (asynchronous readback and a sender thread)
  UI/          Dear ImGui menu
app/src/main/java/   MainActivity (extends SDL's SDLActivity)
app/tests/           desktop tests
app/externals/       SDL3, glm, Dear ImGui
```

## Planned

- Web version (the code already uses only OpenGL ES 3.0 / WebGL 2 and SDL3).
- Saving and loading projects (`.lvskt`).

## Requirements / Credits

- [NDI SDK](https://ndi.video/) – real-time streaming of the canvas (not included; see its license).
- [SDL3](https://libsdl.org/) – window, OpenGL context, input and Android lifecycle.
- [Dear ImGui](https://github.com/ocornut/imgui) – user interface.
- [glm](https://github.com/g-truc/glm) – math.
- [stb_image / stb_image_write](https://github.com/nothings/stb) – brush textures and PNG export.
