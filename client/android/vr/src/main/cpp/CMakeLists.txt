cmake_minimum_required(VERSION 3.22.1)
project(xvrvr)

# The OpenXR loader AAR is consumed through prefab (enabled in build.gradle), which is
# what exposes both its headers and the prebuilt .so to CMake. Vendoring a copy instead
# would let the headers drift from the loader Gradle actually links.
find_package(OpenXR REQUIRED CONFIG)

# native_app_glue ships as source with the NDK rather than as a prebuilt library, so it
# has to be compiled into the project.
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
)

add_library(xvrvr SHARED
    main.cpp
    OpenXrSession.cpp
    GlRenderer.cpp
    Input.cpp
)

target_include_directories(xvrvr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ANativeActivity_onCreate is referenced only by the OS, so the linker would otherwise
# discard it and the app would start and immediately do nothing.
set_target_properties(xvrvr PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate"
)

target_link_libraries(xvrvr
    native_app_glue
    OpenXR::openxr_loader
    android
    log
    EGL
    GLESv3
)
