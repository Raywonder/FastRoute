cmake_minimum_required(VERSION 3.20)
project(FastRoute C CXX)   # C for the vendored denoisers

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 7 SP1 is the floor. Two things follow from that:
#  * the CRT is linked statically, so the machine needs no redistributable and
#    no Universal CRT update to run this;
#  * anything newer than Windows 7 is resolved at run time rather than imported
#    (see ProcessLoopbackAvailable), because a single unresolvable import stops
#    the process from starting at all.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Which commit this is, when there is one to find. The updater compares
# commits rather than version strings where it can: two builds of 0.6 are
# otherwise indistinguishable, and between releases that is every build.
find_package(Git QUIET)
set(FASTROUTE_COMMIT "")
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE FASTROUTE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

add_subdirectory(third_party)

add_executable(FastRoute WIN32
    src/main.cpp
    src/util.cpp
    src/model.cpp
    src/json.cpp
    src/ui_sessions.cpp
    src/ui_session.cpp
    src/ui_dialogs.cpp
    src/ui_slider.cpp
    src/ui_plugin.cpp
    src/tray.cpp
    src/scheduler.cpp
    src/updater.cpp
    src/audio/devices.cpp
    src/audio/asiodev.cpp
    src/audio/asioio.cpp
    src/audio/normalise.cpp
    src/audio/buffer.cpp
    src/audio/capture.cpp
    src/audio/foreground.cpp
    src/audio/engine.cpp
    src/audio/denoise.cpp
    src/audio/eq.cpp
    src/audio/dynamics.cpp
    src/audio/pitch.cpp
    src/audio/plugfx.cpp
    src/plugins/clap_host.cpp
    src/plugins/vst3_host.cpp
    src/plugins/scan.cpp
    src/audio/stream.cpp
    src/audio/net.cpp
    src/audio/netcodec.cpp
    src/audio/netsend.cpp
    src/audio/netrecv.cpp
    src/audio/recorder.cpp
    src/audio/oggout.cpp
    src/audio/rtmp.cpp
    src/audio/aacenc.cpp
    src/audio/icecast.cpp
    src/audio/streamenc.cpp
    src/audio/streamout.cpp
    src/aaa.cpp
    src/fastroute.rc
)

target_include_directories(FastRoute PRIVATE src)
target_compile_definitions(FastRoute PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (FASTROUTE_COMMIT)
    target_compile_definitions(FastRoute PRIVATE
        FASTROUTE_BUILD_COMMIT="${FASTROUTE_COMMIT}")
endif()
# mmdevapi is deliberately absent: its only use here is
# ActivateAudioInterfaceAsync, which Windows 7 does not export.
target_link_libraries(FastRoute PRIVATE
    comctl32 oleacc ole32 oleaut32 uuid propsys shlwapi avrt user32 gdi32 advapi32 shell32
    mfplat mfuuid mfreadwrite mf ole32
    delayimp
    rnnoise speexdsp signalsmith_stretch FLAC asiosdk
    clap_headers vst3_pluginterfaces
    vorbis vorbisfile vorbisenc ogg opus opusfile
    winhttp ws2_32 iphlpapi crypt32)

if (MSVC)
    target_compile_options(FastRoute PRIVATE /W3 /permissive-)
    target_link_options(FastRoute PRIVATE
        # Media Foundation is only touched once MediaFoundationPresent() has
        # confirmed the DLLs load, so delay loading them means an edition
        # without them (Windows N without the Media Feature Pack) still runs -
        # it just cannot offer FLAC or MP3.
        /DELAYLOAD:mfplat.dll
        /DELAYLOAD:mfreadwrite.dll
        /DELAYLOAD:mf.dll
        # Windows 7.
        /SUBSYSTEM:WINDOWS,6.01)
endif()

# A command line check of Stream Out's encoders and protocols, not shipped:
# publishes a few seconds of a stereo tone to a real server. Off by default so
# the release build is unchanged; configure with -DFASTROUTE_SELFTEST=ON.
option(FASTROUTE_SELFTEST "Build the Stream Out self test" OFF)
if (FASTROUTE_SELFTEST)
    add_executable(streamout_selftest
        tools/streamout_selftest.cpp
        src/util.cpp
        src/audio/rtmp.cpp
        src/audio/aacenc.cpp
        src/audio/icecast.cpp
        src/audio/streamenc.cpp
        src/audio/oggout.cpp)
    target_include_directories(streamout_selftest PRIVATE src)
    target_compile_definitions(streamout_selftest PRIVATE UNICODE _UNICODE NOMINMAX
                               WIN32_LEAN_AND_MEAN)
    target_link_libraries(streamout_selftest PRIVATE
        mfplat mfuuid mf ole32 oleaut32 oleacc uuid shell32 user32 crypt32 ws2_32
        vorbis vorbisenc ogg opus speexdsp)
endif()
