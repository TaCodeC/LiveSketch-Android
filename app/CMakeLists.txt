cmake_minimum_required(VERSION 3.22)
project(livesketch LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTERNALS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/externals)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp)
set(ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets)

# -----------------------------------------------------------------------------
# NDI (opcional)
# La ruta del SDK se toma, en este orden, de -DNDI_SDK_DIR (Gradle la pasa desde
# ndi.sdk.dir en local.properties), de la variable de entorno NDI_SDK_DIR o de la
# ruta que usaba el CMakeLists original. Si no se encuentra, se compila sin NDI.
# -----------------------------------------------------------------------------
option(LIVESKETCH_WITH_NDI "Compilar la salida NDI (requiere el NDI SDK)" ON)
set(NDI_SDK_DIR "" CACHE PATH "Carpeta del NDI SDK (con include/ y lib/)")
if(NOT NDI_SDK_DIR AND DEFINED ENV{NDI_SDK_DIR})
    set(NDI_SDK_DIR "$ENV{NDI_SDK_DIR}")
endif()
if(NOT NDI_SDK_DIR AND EXISTS "/Users/tacode/NDI_Android")
    set(NDI_SDK_DIR "/Users/tacode/NDI_Android")
endif()

set(LIVESKETCH_NDI_ENABLED OFF)
if(LIVESKETCH_WITH_NDI AND NDI_SDK_DIR)
    if(ANDROID)
        set(NDI_LIBRARY "${NDI_SDK_DIR}/lib/${ANDROID_ABI}/libndi.so")
    else()
        find_library(NDI_LIBRARY NAMES ndi
                     PATHS "${NDI_SDK_DIR}/lib"
                     PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
                     NO_DEFAULT_PATH)
    endif()
    if(EXISTS "${NDI_SDK_DIR}/include/Processing.NDI.Lib.h" AND NDI_LIBRARY AND EXISTS "${NDI_LIBRARY}")
        set(LIVESKETCH_NDI_ENABLED ON)
    endif()
endif()
if(LIVESKETCH_WITH_NDI AND NOT LIVESKETCH_NDI_ENABLED)
    message(WARNING "NDI SDK no encontrado (NDI_SDK_DIR='${NDI_SDK_DIR}'): se compila sin salida NDI.")
endif()

# -----------------------------------------------------------------------------
# SDL3 (submódulo en externals/SDL)
# -----------------------------------------------------------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXTERNALS_DIR}/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL)

# OpenGL ES 3.0. En Linux, libGLESv2 (Mesa/libglvnd) incluye las funciones de GLES 3.
if(ANDROID)
    find_library(GLES_LIBRARY GLESv3 REQUIRED)
else()
    find_library(GLES_LIBRARY GLESv2 REQUIRED)
endif()

# -----------------------------------------------------------------------------
# Dear ImGui (backends SDL3 + OpenGL3)
# -----------------------------------------------------------------------------
set(IMGUI_DIR ${EXTERNALS_DIR}/imgui)
add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 ${GLES_LIBRARY})

# -----------------------------------------------------------------------------
# LiveSketch
# -----------------------------------------------------------------------------
add_library(livesketch_core STATIC
        ${SRC_DIR}/App/App.cpp
        ${SRC_DIR}/App/AppInput.cpp
        ${SRC_DIR}/Canvas/Brush.cpp
        ${SRC_DIR}/Canvas/Camera.cpp
        ${SRC_DIR}/Canvas/Canvas.cpp
        ${SRC_DIR}/Canvas/CanvasView.cpp
        ${SRC_DIR}/Canvas/Compositor.cpp
        ${SRC_DIR}/Canvas/LayerStack.cpp
        ${SRC_DIR}/Gfx/GLObjects.cpp
        ${SRC_DIR}/Gfx/Shader.cpp
        ${SRC_DIR}/IO/Assets.cpp
        ${SRC_DIR}/NDI/NdiOutput.cpp
        ${SRC_DIR}/ThirdParty/stb_impl.cpp
        ${SRC_DIR}/UI/Menu.cpp
)
target_include_directories(livesketch_core PUBLIC ${SRC_DIR} ${EXTERNALS_DIR}/glm)
target_compile_options(livesketch_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Código de terceros: sin avisos.
set_source_files_properties(${SRC_DIR}/ThirdParty/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(livesketch_core PUBLIC imgui SDL3::SDL3 ${GLES_LIBRARY})

if(ANDROID)
    target_link_libraries(livesketch_core PUBLIC log android)
else()
    # En escritorio los assets se leen directamente de la carpeta del proyecto.
    target_compile_definitions(livesketch_core PUBLIC LIVESKETCH_ASSETS_DIR="${ASSETS_DIR}")
endif()

if(LIVESKETCH_NDI_ENABLED)
    message(STATUS "LiveSketch: salida NDI activada (${NDI_LIBRARY})")
    target_sources(livesketch_core PRIVATE ${SRC_DIR}/NDI/NdiSender.cpp)
    target_compile_definitions(livesketch_core PUBLIC LIVESKETCH_WITH_NDI=1)
    target_include_directories(livesketch_core PRIVATE ${NDI_SDK_DIR}/include)
    target_link_libraries(livesketch_core PUBLIC ${NDI_LIBRARY})
else()
    target_sources(livesketch_core PRIVATE ${SRC_DIR}/NDI/NoNdi.cpp)
endif()

if(ANDROID)
    # SDLActivity carga libSDL3.so y liblivesketch.so (ver MainActivity.getLibraries).
    add_library(livesketch SHARED ${SRC_DIR}/main.cpp)
else()
    add_executable(livesketch ${SRC_DIR}/main.cpp)
endif()
target_link_libraries(livesketch PRIVATE livesketch_core)
