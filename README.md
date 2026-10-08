# LiveSketch

This Android app was developed to be actively used by students during class sessions at my university (unofficial request). While it is still in early development, it is already functional and usable for drawing and testing purposes.

LiveSketch allows users to draw on a multi-layer canvas and stream the result in real time using NDI (Network Device Interface). This makes it suitable for live demos, classroom activities, or experimental creative workflows where remote or shared visuals are needed.

## Features

- Procreate-style layout with an iOS look: floating frosted-glass bars over the canvas (the glass blurs the drawing behind it), a sidebar with brush size, eyedropper, opacity, undo and redo, and panels that open from the bars: popovers on tablets and desktop, bottom sheets on phones held upright. Dark theme, Inter font and Lucide icons. The interface size (small, normal, large) and the side of the sidebar can be changed in Actions.
- Multi-layer canvas: add, delete, rename, move up/down, duplicate, merge down and clear layers, with per-layer visibility, opacity and live thumbnails.
- Pressure-sensitive brushes (four brush textures, each with a preview stroke), color wheel with hex input, recent colors and a palette. The brush and the eraser keep their own brush, size and opacity; the stylus eraser tip uses the eraser settings.
- Undo and redo for strokes and layer changes.
- Eyedropper, with a loupe that shows the color under it next to the current one.
- New canvas card: sizes by category (video and NDI, screen, social media, paper, comics) and your own saved sizes, or a custom size in px, mm, cm or inches with its resolution in ppi; the canvas name, its color profile and its background (white, any color or transparent), with a live summary of the print size and how many layers fit.
- Color profiles: each canvas is sRGB (the default) or Display P3, which reaches more intense reds, greens and oranges. With a P3 canvas the screen switches to P3 where it can (Android 9 or later on a wide-gamut screen, and Chrome or Safari on a P3 display); elsewhere the view converts the colors. NDI always receives the colors converted to sRGB.
- The background is a color of the canvas, not a layer: the "Color de fondo" row at the bottom of the Layers panel changes it without touching the drawing, or hides it for a transparent PNG and NDI with alpha.
- Actions > Canvas > "Propiedades": rename the canvas, change its ppi without resampling, change its background, convert it to the other color profile (the drawing keeps its look; it can be undone), and see its size in pixels and on paper, layers, memory, dates, drawing time and strokes.
- NDI output of the full canvas (source name `LiveSketch`), independent of the on-screen zoom. The NDI capsule turns red while live and shows the number of receivers.
- "Guardar PNG" saves the full canvas to Downloads, named after the canvas (`LiveSketch_YYYYMMDD_HHMMSS.png` if it has no name), with its resolution in ppi and its color profile inside the file (for Display P3, an ICC profile and the PNG `cICP` chunk), so it prints at the canvas size and shows its colors right. Hiding the background color makes it transparent.
- The Android back button closes the open panel; with everything closed, it asks before exiting.
- It also runs in a web browser (WebGL 2), without NDI: see [Web](#web-browser).

### Gestures and shortcuts

| Action | Touch and stylus | Mouse and keyboard |
|---|---|---|
| Draw | Stylus, or one finger if "Dibujar con el dedo" is on | Left button |
| Move and zoom | Two fingers (one finger moves when finger drawing is off) | Right or middle button; wheel |
| Undo / redo | Tap with two / three fingers, or the sidebar buttons (hold to repeat) | Ctrl+Z / Ctrl+Shift+Z or Ctrl+Y |
| Eyedropper | Sidebar button, or hold a finger still when finger drawing is on | Alt+click, or the sidebar button |
| Brush / eraser | Top-right bar; tap the selected tool again for its brushes | B / E |
| Brush size | Sidebar slider | [ and ] |
| Close a panel | Tap outside it, or the Android back button | Esc |

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

### Web (browser)

The same code builds for the web with [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) and runs on WebGL 2. A browser cannot send NDI, so that option is off, and "Guardar PNG" downloads the image.

```sh
# Emscripten SDK, once
git clone https://github.com/emscripten-core/emsdk.git
./emsdk/emsdk install latest && ./emsdk/emsdk activate latest
source ./emsdk/emsdk_env.sh

emcmake cmake -S app -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
```

The result is a single file, `build-web/LiveSketch.html`, with the app and its brushes inside. Open it with a double click, or serve it from any web server (for example `npx serve build-web`). It needs a browser with WebGL 2: current Chrome, Edge, Firefox or Safari.

With `-DLIVESKETCH_WEB_SINGLE_FILE=OFF` the build writes `LiveSketch.html`, `.js` and `.wasm` separately, which have to be served over HTTP. With `-DLIVESKETCH_BUILD_TESTS=ON` it also builds `livesketch_tests.html` (all tests except NDI), which shows the results on the page when served.

## Project layout

```
app/src/main/cpp/
  main.cpp     SDL entry points (main callbacks)
  App/         lifecycle, GL context, frame pacing, input routing and the screen's color profile
  Canvas/      layers, brush, compositing, camera and canvas view
  Gfx/         OpenGL helpers (RAII objects, shaders, pixel conversion) and color profiles
  IO/          assets, PNG export and ICC profiles
  NDI/         NDI output (asynchronous readback and a sender thread)
  UI/          interface: iOS-style controls drawn with Dear ImGui, bars, panels,
               dialogs, animations and the blurred backdrop of the glass
app/src/main/assets/ brush textures and interface fonts
app/src/main/java/   MainActivity (extends SDL's SDLActivity)
app/src/web/         page for the web version
app/tests/           desktop tests
app/externals/       SDL3, glm, Dear ImGui
tools/fonts/         script that builds the font subsets and UI/Icons.h
```

## Planned

- Saving and loading projects (`.lvskt`).

## Requirements / Credits

- [NDI SDK](https://ndi.video/) – real-time streaming of the canvas (not included; see its license).
- [SDL3](https://libsdl.org/) – window, OpenGL context, input and Android lifecycle.
- [Dear ImGui](https://github.com/ocornut/imgui) – user interface engine.
- [Inter](https://rsms.me/inter/) – interface font (SIL Open Font License 1.1, `app/src/main/assets/fonts/Inter-LICENSE.txt`).
- [Lucide](https://lucide.dev/) – icons (ISC License, `app/src/main/assets/fonts/Lucide-LICENSE.txt`).
- [glm](https://github.com/g-truc/glm) – math.
- [stb_image](https://github.com/nothings/stb) – brush textures.
- [libdeflate](https://github.com/ebiggers/libdeflate) – PNG compression (MIT License, `app/src/main/cpp/ThirdParty/libdeflate/COPYING`).
