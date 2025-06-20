cmake_minimum_required(VERSION 3.22)
project(livesketch LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Para asegurar que se incluya ANativeActivity_onCreate
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

# Rutas NDK y dependencias externas
set(NDK_PATH ${ANDROID_NDK})
set(NATIVE_APP_GLUE_DIR ${NDK_PATH}/sources/android/native_app_glue)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/externals/imgui)

# Ruta de NDI SDK
set(NDI_DIR /Users/tacode/NDI_Android)
include_directories(
        ${NATIVE_APP_GLUE_DIR}
        ${CMAKE_SOURCE_DIR}/externals/glm
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${NDI_DIR}/include
)

# Biblioteca glue y ImGui
add_library(android_native_app_glue STATIC ${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c)
add_library(imgui
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_android.cpp
)

enable_language(CXX)

# Biblioteca principal y NDI sender
add_library(livesketch SHARED
        src/main/cpp/main.cpp
        src/main/cpp/Renderer/Renderer.cpp
        src/main/cpp/layers/Layer.cpp
        src/main/cpp/layers/LayerManager.cpp
        src/main/cpp/Brush/Brush.cpp
        src/main/cpp/Brush/stb_image.h
        src/main/cpp/DevUI/IMGUIMenu.h
        src/main/cpp/DevUI/IMGUIMenu.cpp
        src/main/cpp/NDI/NDISender.h       # header
        src/main/cpp/NDI/NDISender.cpp     # implementación
        src/main/cpp/NDIThreadSender.h
        src/main/cpp/NDIThreadSender.cpp
        src/main/cpp/FileManager/FileManager.h
        src/main/cpp/FileManager/FileManager.cpp
)
#add_library(miniz STATIC src/main/cpp/FileManager/miniz.c)


# Bibliotecas de sistema
find_library(log-lib log)
find_library(android-lib android)
find_library(EGL-lib EGL)
find_library(GLESv3-lib GLESv3)

# Detección de ABI para la librería NDI
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(NDI_LIB ${NDI_DIR}/lib/arm64-v8a/libndi.so)
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(NDI_LIB ${NDI_DIR}/lib/armeabi-v7a/libndi.so)
elseif(${ANDROID_ABI} STREQUAL "x86_64")
    set(NDI_LIB ${NDI_DIR}/lib/x86_64/libndi.so)
else()
    message(FATAL_ERROR "Unsupported ABI: ${ANDROID_ABI}")
endif()

target_link_libraries(livesketch
        PUBLIC
        imgui     # si otros targets van a usar ImGui
        PRIVATE
        #miniz
        ${NDI_LIB}
        android_native_app_glue
        ${log-lib}
        ${android-lib}
        ${EGL-lib}
        ${GLESv3-lib}
)


# Incluir imgui como dependencia pública si fuera necesario
# target_link_libraries(livesketch PUBLIC imgui)
