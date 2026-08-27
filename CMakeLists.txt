cmake_minimum_required(VERSION 3.21)
project(VideoEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---------------------------------------------------------------------------
# Qt6
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia Concurrent Network)

# ---------------------------------------------------------------------------
# libmpv (used for the preview player)
#
# On Windows via vcpkg:  vcpkg install mpv
# On Linux:              apt install libmpv-dev
# On macOS:               brew install mpv
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MPV QUIET IMPORTED_TARGET mpv)
endif()

if(NOT MPV_FOUND)
    # Fallback for vcpkg / manual installs where pkg-config isn't set up.
    find_path(MPV_INCLUDE_DIR mpv/client.h)
    find_library(MPV_LIBRARY NAMES mpv libmpv mpv.dll.a)
    if(MPV_INCLUDE_DIR AND MPV_LIBRARY)
        add_library(PkgConfig::MPV INTERFACE IMPORTED)
        set_target_properties(PkgConfig::MPV PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${MPV_LIBRARY}")
    else()
        message(FATAL_ERROR "libmpv not found. Install it (vcpkg install mpv) or set MPV_INCLUDE_DIR/MPV_LIBRARY manually.")
    endif()
endif()

# ---------------------------------------------------------------------------
# FFmpeg (used for import/trim/export, i.e. the actual editing pipeline)
#
# On Windows via vcpkg:  vcpkg install ffmpeg
# On Linux:              apt install libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev
# ---------------------------------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavfilter
        libavutil
        libswscale
        libswresample
    )
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
add_executable(VideoEditor
    src/main.cpp
    src/Theme.cpp
    include/Theme.h
    src/MainWindow.cpp
    include/MainWindow.h
    src/PlayerWidget.cpp
    include/PlayerWidget.h
    src/Timeline.cpp
    include/Timeline.h
    src/Project.cpp
    include/Project.h
    src/FFmpegExporter.cpp
    include/FFmpegExporter.h
    src/Transcriber.cpp
    include/Transcriber.h
    src/MediaProbe.cpp
    include/MediaProbe.h
    src/WaveformGenerator.cpp
    include/WaveformGenerator.h
    src/TrackHeaderPanel.cpp
    include/TrackHeaderPanel.h
    include/TimelineMetrics.h
    src/ThumbnailGenerator.cpp
    include/ThumbnailGenerator.h
    src/AudioPlayer.cpp
    include/AudioPlayer.h
    src/KlipyClient.cpp
    include/KlipyClient.h
    src/KlipyPanel.cpp
    include/KlipyPanel.h
    src/OverlayStageWidget.cpp
    include/OverlayStageWidget.h
    src/OverlayInspectorPanel.cpp
    include/OverlayInspectorPanel.h
    src/MediaBrowserPanel.cpp
    include/MediaBrowserPanel.h
    src/MediaThumbnailProxyModel.cpp
    include/MediaThumbnailProxyModel.h
    src/OverlayImageLoader.cpp
    include/OverlayImageLoader.h
)

target_include_directories(VideoEditor PRIVATE include)

target_link_libraries(VideoEditor PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::Network
    PkgConfig::MPV
)

# --- Windows: copy Qt plugins next to the exe -------------------------------
# vcpkg doesn't ship windeployqt, so the plugins Qt loads at runtime have to be
# placed manually. Without these the app builds and launches fine but silently
# loses features: no `tls` means HTTPS fails, no `imageformats` means GIF and
# WebP previews come back blank.
if(WIN32)
    set(QT_PLUGIN_ROOT "C:/Users/steve/vcpkg/installed/x64-windows/debug/Qt6/plugins")
    foreach(PLUGIN_DIR platforms imageformats tls styles)
        if(EXISTS "${QT_PLUGIN_ROOT}/${PLUGIN_DIR}")
            add_custom_command(TARGET VideoEditor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QT_PLUGIN_ROOT}/${PLUGIN_DIR}"
                    "$<TARGET_FILE_DIR:VideoEditor>/${PLUGIN_DIR}"
                COMMENT "Copying Qt ${PLUGIN_DIR} plugins")
        endif()
    endforeach()
endif()

if(FFMPEG_FOUND)
    target_link_libraries(VideoEditor PRIVATE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# ffmpeg EXECUTABLE (needed at runtime by the exporter)
#
# Distinct from the FFmpeg libraries linked above, and easy to conflate: the
# app links libavformat/libavcodec for probing, waveforms and thumbnails, but
# FFmpegExporter drives the ffmpeg BINARY as a subprocess (see the rationale at
# the top of FFmpegExporter.h). Linking the libraries successfully therefore
# says nothing about whether the tool is present — which is exactly why the
# "ffmpeg not found" dialog can appear on a build that compiled cleanly.
#
# vcpkg's ffmpeg port already builds the executable and installs it under
# tools/ffmpeg, so on this setup it's almost certainly on disk already and just
# never got copied next to the app. Copying it here mirrors what's done for the
# Qt plugins above and makes the export work from a fresh clone without anyone
# having to know any of the above.
#
# Not fatal when missing: the app runs fine without it, and everything except
# export works. The exporter looks on PATH too, so a system install still gets
# picked up at runtime.
# ---------------------------------------------------------------------------
find_program(FFMPEG_EXECUTABLE
    NAMES ffmpeg
    HINTS
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/ffmpeg"
        "${CMAKE_PREFIX_PATH}/tools/ffmpeg"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/ffmpeg"
    PATH_SUFFIXES tools/ffmpeg bin
)

if(FFMPEG_EXECUTABLE)
    message(STATUS "ffmpeg executable found for export: ${FFMPEG_EXECUTABLE}")
    add_custom_command(TARGET VideoEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_EXECUTABLE}"
            "$<TARGET_FILE_DIR:VideoEditor>"
        COMMENT "Copying ffmpeg executable next to VideoEditor (needed for export)")
else()
    message(WARNING
        "ffmpeg executable NOT found — everything works except EXPORT, which "
        "will show an 'ffmpeg not found' dialog.\n"
        "  Fix by either:\n"
        "    * vcpkg install ffmpeg[core,x264]:x64-windows   (then reconfigure), or\n"
        "    * download a build from https://www.gyan.dev/ffmpeg/builds/ and put "
        "ffmpeg.exe next to VideoEditor.exe, or\n"
        "    * pass -DFFMPEG_EXECUTABLE=C:/path/to/ffmpeg.exe to cmake.")
endif()

# ---------------------------------------------------------------------------
# whisper.cpp (local, offline transcription — no cloud/network dependency
# at runtime, matching the rest of the app)
#
# Pulled in via FetchContent rather than a vcpkg port — much lighter than
# hunting for another port name, and whisper.cpp is explicitly designed to
# be consumed this way. This DOES mean the first configure will clone the
# whole repo and build it, which takes real time (much lighter than the
# QtPdf/Chromium situation, but not instant) — that's expected, not stuck.
#
# GPU: Vulkan (GGML_VULKAN) is used when available rather than CUDA/ROCm —
# unlike those, Vulkan runs on the SAME standard AMD graphics driver already
# installed for normal GPU use, no separate compute driver stack to fight
# with, which is exactly the kind of friction hit before with ROCm/CUDA on
# this machine. Requires the Vulkan SDK at BUILD time
# (https://vulkan.lunarg.com/sdk/home#windows) — if it's not found, this
# falls back to CPU-only automatically rather than failing the whole build.
#
# IMPORTANT for this CPU specifically: GGML_VULKAN combined with AVX512 is a
# documented crash risk on Intel 12th/13th-gen CPUs (confirmed via current
# whisper.cpp community docs) — AVX512 is explicitly disabled below for
# exactly that reason, not as a generic precaution.
#
# Honest expectation-setting: real-world reports on Windows show Vulkan's
# actual speedup varies a lot by GPU/driver maturity — Linux's Mesa RADV
# driver is reported as dramatically more efficient than Windows' Vulkan
# driver stack for this workload in at least one documented case. This is
# worth trying, not a guaranteed large win — if it doesn't help, dropping
# back to CPU-only (just reconfigure with -DENABLE_WHISPER_VULKAN=OFF) costs
# nothing.
#
# A model file is a SEPARATE download this doesn't handle — get one from
# https://huggingface.co/ggerganov/whisper.cpp/tree/main (ggml-base.en.bin
# is a good starting point — decent accuracy, ~140MB, English-only). The
# app looks for it at <exe dir>/models/ggml-base.en.bin first; if not
# found, it prompts once via a file picker and remembers your choice.
# ---------------------------------------------------------------------------
option(ENABLE_WHISPER_VULKAN "Build whisper.cpp with Vulkan GPU acceleration (requires the Vulkan SDK)" ON)
if(ENABLE_WHISPER_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        message(STATUS "Vulkan SDK found — building whisper.cpp with GPU acceleration")
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
        set(GGML_AVX512 OFF CACHE BOOL "" FORCE) # crash risk on Intel 12th/13th-gen combined with Vulkan — see note above
    else()
        message(WARNING "Vulkan SDK not found — building whisper.cpp CPU-only. "
                         "Install it from https://vulkan.lunarg.com/sdk/home#windows and reconfigure to enable GPU acceleration.")
    endif()
endif()

include(FetchContent)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    whisper_cpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.7.2
)
FetchContent_MakeAvailable(whisper_cpp)
target_link_libraries(VideoEditor PRIVATE whisper)

# NOTE: no manual windeployqt step needed here — vcpkg's toolchain already
# auto-copies all required Qt (and other) DLLs next to the built .exe via
# its built-in "applocal deps" mechanism (VCPKG_APPLOCAL_DEPS, on by
# default). You can see it happening in the build log as a series of
# "<dll> -> ... done" lines.
#
# whisper.cpp/ggml, however, come from FetchContent rather than vcpkg, so
# vcpkg's applocal-deps mechanism doesn't know about their DLLs (whisper.dll
# and, depending on the ggml build, separate ggml*.dll files) — they'd
# otherwise get built somewhere inside the build tree and never make it
# next to VideoEditor.exe, causing a "whisper.dll was not found" error at
# launch. $<TARGET_RUNTIME_DLLS:...> (CMake 3.21+) is the modern, correct
# fix for exactly this: it resolves to every DLL the target actually
# depends on at runtime, transitively, without needing to know individual
# ggml/whisper target names (which vary across versions) — copying them
# next to the exe on every build, same as vcpkg does for its own DLLs.
#
# This also means enabling Vulkan above needs NO further changes here —
# the additional ggml-vulkan.dll it produces gets picked up automatically
# by the same generic mechanism.
add_custom_command(TARGET VideoEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoEditor>
        $<TARGET_FILE_DIR:VideoEditor>
    COMMAND_EXPAND_LISTS
)
