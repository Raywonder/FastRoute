# ---------------------------------------------------------------------------
# Third party denoisers.
#
# Both are fetched at configure time and pinned to an exact commit, and both
# are compiled here from an explicit source list rather than through their own
# build systems: they ship autotools, which is no use to us, and we only want a
# small part of each anyway.
# ---------------------------------------------------------------------------
include(FetchContent)

if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)   # keep the single argument FetchContent_Populate
endif()

# --- RNNoise ---------------------------------------------------------------
# v0.1.1 deliberately, not master: this is the last release that commits the
# trained weights (src/rnn_data.c). Master downloads a model tarball at build
# time, which would make the build depend on the network every time.
FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG        6cbfd53eb348a8d394e0757b4025c6ded34eb2b6   # v0.1.1
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(rnnoise)
if (NOT rnnoise_POPULATED)
    FetchContent_Populate(rnnoise)
endif()

# MSVC's C compiler has no variable length arrays, and v0.1.1 uses five of
# them. Rather than patch the upstream checkout in place - which fights with
# FetchContent every time it re-populates - the two affected files are copied
# into the build tree with those declarations rewritten to _alloca, which is
# the same allocation with the same lifetime. Regenerated on every configure,
# so it cannot drift out of step with the source.
set(RNNOISE_FIXED ${CMAKE_CURRENT_BINARY_DIR}/rnnoise_msvc)
file(MAKE_DIRECTORY ${RNNOISE_FIXED})

set(RNNOISE_VLA_FIXES
    "opus_val16 x_lp4[len>>2]|opus_val16 *x_lp4 = (opus_val16*)_alloca(sizeof(opus_val16)*(len>>2))"
    "opus_val16 y_lp4[lag>>2]|opus_val16 *y_lp4 = (opus_val16*)_alloca(sizeof(opus_val16)*(lag>>2))"
    "opus_val32 xcorr[max_pitch>>1]|opus_val32 *xcorr = (opus_val32*)_alloca(sizeof(opus_val32)*(max_pitch>>1))"
    "opus_val32 yy_lookup[maxperiod+1]|opus_val32 *yy_lookup = (opus_val32*)_alloca(sizeof(opus_val32)*(maxperiod+1))"
    "opus_val16 xx[n]|opus_val16 *xx = (opus_val16*)_alloca(sizeof(opus_val16)*(n))")

foreach(f pitch.c celt_lpc.c)
    file(READ ${rnnoise_SOURCE_DIR}/src/${f} _text)
    foreach(fix ${RNNOISE_VLA_FIXES})
        string(REPLACE "|" ";" _pair "${fix}")
        list(GET _pair 0 _from)
        list(GET _pair 1 _to)
        string(REPLACE "${_from}" "${_to}" _text "${_text}")
    endforeach()
    file(WRITE ${RNNOISE_FIXED}/${f} "#include <malloc.h>
${_text}")
endforeach()

add_library(rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/rnn_data.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${RNNOISE_FIXED}/pitch.c
    ${RNNOISE_FIXED}/celt_lpc.c)
# rnn_reader.c is left out on purpose: it includes config.h unconditionally and
# only exists to load models from disk, which we never do.
target_include_directories(rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)
target_include_directories(rnnoise PRIVATE ${rnnoise_SOURCE_DIR}/src)
target_compile_definitions(rnnoise PRIVATE
    RNNOISE_EXPORT= _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(rnnoise PRIVATE /W0 /wd4244 /wd4267)
endif()

# --- SpeexDSP preprocessor -------------------------------------------------
FetchContent_Declare(speexdsp
    GIT_REPOSITORY https://github.com/xiph/speexdsp.git
    GIT_TAG        7a158783df74efe7c2d1c6ee8363c1e695c71226
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(speexdsp)
if (NOT speexdsp_POPULATED)
    FetchContent_Populate(speexdsp)
endif()

add_library(speexdsp STATIC
    ${speexdsp_SOURCE_DIR}/libspeexdsp/preprocess.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/filterbank.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/fftwrap.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fft.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fftr.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/smallft.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/resample.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/mdf.c)
# mdf.c is the echo canceller, which we do not use, but preprocess.c calls
# speex_echo_get_residual() from it and the linker wants the symbol.
target_include_directories(speexdsp PUBLIC ${speexdsp_SOURCE_DIR}/include)
target_include_directories(speexdsp PRIVATE ${speexdsp_SOURCE_DIR}/libspeexdsp)
target_compile_definitions(speexdsp PRIVATE
    FLOATING_POINT USE_KISS_FFT EXPORT= _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(speexdsp PRIVATE /W0 /wd4244 /wd4267)
endif()

# --- Signalsmith Stretch ---------------------------------------------------
# The pitch shifter. Header only and MIT, so nothing is compiled here: the
# target exists only to put the checkout on the include path.
#
# 1.1.0 deliberately, not main. This tag is self-contained - signalsmith-stretch.h
# and the dsp/ headers beside it, and nothing else. Later versions moved the
# FFTs out into a separate Signalsmith Linear repository, which would make this
# a second checkout to pin and keep in step for no gain here. The repository's
# one submodule (cmd/util) belongs to its command line tool and is not fetched.
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        44c8f865af9da8c29cc4a70a2d5a3ec83639c711   # 1.1.0
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES "")
FetchContent_GetProperties(signalsmith_stretch)
if (NOT signalsmith_stretch_POPULATED)
    FetchContent_Populate(signalsmith_stretch)
endif()
add_library(signalsmith_stretch INTERFACE)
target_include_directories(signalsmith_stretch INTERFACE ${signalsmith_stretch_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Stream codecs: Ogg, Vorbis, Opus
#
# Windows decodes MP3, AAC and (since Windows 10) FLAC on its own, so the
# stream block hands those to Media Foundation. It does not reliably decode
# Ogg Vorbis or Opus: this machine has no Vorbis decoder at all, and Windows 7
# has neither. Those two are therefore built here from source, so a stream in
# either format works the same everywhere rather than on whichever machines
# happen to have the Store codec pack.
#
# vorbisfile and opusfile are taken along with them: both wrap the Ogg framing
# and hand back PCM through a callback interface, which is exactly the shape a
# network stream needs and a great deal less code than demuxing Ogg by hand.
# ---------------------------------------------------------------------------

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# libogg, libvorbis and libopusfile still declare cmake_minimum_required(2.8),
# which current CMake refuses outright. They build fine; it is the declaration
# that is stale, so this floors it rather than patching four upstream files.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
# Upstream test programs are built by default and are several minutes of
# compiling nobody here waits for. Their own OPUS_BUILD_TESTING switch does not
# reach the CTest ones; this does.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ogg)
# libvorbis and libopusfile both look for the installed package rather than a
# sibling target, so the alias they expect is provided here.
if (NOT TARGET Ogg::ogg)
    add_library(Ogg::ogg ALIAS ogg)
endif()
set(OGG_INCLUDE_DIR "${ogg_SOURCE_DIR}/include;${ogg_BINARY_DIR}/include" CACHE STRING "" FORCE)
set(OGG_LIBRARY ogg CACHE STRING "" FORCE)

FetchContent_Declare(vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vorbis)

FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.4
    GIT_SHALLOW TRUE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)
if (NOT TARGET Opus::opus)
    add_library(Opus::opus ALIAS opus)
endif()

# opusfile ships autotools and a CMakeLists that wants pkg-config and OpenSSL.
# It is four source files, so it is compiled here directly instead - with the
# HTTP layer off, because FastRoute does its own fetching and that layer would
# drag OpenSSL in behind it.
FetchContent_Declare(opusfile
    GIT_REPOSITORY https://github.com/xiph/opusfile.git
    GIT_TAG v0.12
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(opusfile)
if (NOT opusfile_POPULATED)
    FetchContent_Populate(opusfile)
endif()
add_library(opusfile STATIC
    ${opusfile_SOURCE_DIR}/src/opusfile.c
    ${opusfile_SOURCE_DIR}/src/info.c
    ${opusfile_SOURCE_DIR}/src/internal.c
    ${opusfile_SOURCE_DIR}/src/stream.c)
target_include_directories(opusfile PUBLIC ${opusfile_SOURCE_DIR}/include)
target_include_directories(opusfile PRIVATE ${opusfile_SOURCE_DIR}/src)
target_compile_definitions(opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_FLOAT_API=0
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(opusfile PUBLIC opus ogg)
if (MSVC)
    target_compile_options(opusfile PRIVATE /W0)
endif()

if (MSVC)
    foreach(t ogg vorbis vorbisfile opus)
        if (TARGET ${t})
            target_compile_options(${t} PRIVATE /W0)
        endif()
    endforeach()
endif()

# --- libFLAC ---------------------------------------------------------------
# For the Net Send and Net Receive blocks. FLAC was already available for
# recording, but only through Media Foundation, which writes a file rather than
# handing back frames - no use at all for putting one frame in each packet.
#
# Only the C library is built: the C++ wrapper is a convenience for file work,
# the programs are the command line tools, and Ogg framing is for files too -
# a FLAC frame goes in a UDP packet on its own here.
FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG        1.4.3
    GIT_SHALLOW    TRUE)
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
# FLAC links Ogg::ogg whenever that target merely exists, and it exists here
# because libvorbis and opusfile both wanted it. The link itself is harmless -
# WITH_OGG being off means none of the Ogg framing is compiled - but FLAC's
# install rules then try to export a target that belongs to somebody else and
# the generate step fails. Nothing here is ever installed, so the export goes.
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flac)
if (MSVC AND TARGET FLAC)
    target_compile_options(FLAC PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# Plugin formats: CLAP and VST3
#
# Both are hosted, not built: FastRoute loads other people's plugins. What is
# needed here is only the interface definitions each format is described by.
#
# CLAP is header only and MIT. VST3 became MIT with version 3.8 in October
# 2025 - before that it was GPLv3 or a proprietary agreement, which is why
# hosting it in a project like this one used to be a licensing decision rather
# than a technical one. Only `pluginterfaces` is taken: the rest of the VST3
# SDK is for writing plugins, and its editor library is of no use to a host.
# ---------------------------------------------------------------------------

FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.9
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(clap)
if (NOT clap_POPULATED)
    FetchContent_Populate(clap)
endif()
add_library(clap_headers INTERFACE)
target_include_directories(clap_headers INTERFACE ${clap_SOURCE_DIR}/include)

# Checked out as .../vst3sdk/pluginterfaces, because that is the name its own
# headers include each other by: "pluginterfaces/base/funknown.h".
FetchContent_Declare(vst3pi
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
    GIT_TAG        v3.8.1_build_84   # 3.8 onwards is the MIT licensed line
    GIT_SHALLOW    TRUE
    SOURCE_DIR     ${CMAKE_BINARY_DIR}/_deps/vst3sdk/pluginterfaces)
FetchContent_GetProperties(vst3pi)
if (NOT vst3pi_POPULATED)
    FetchContent_Populate(vst3pi)
endif()

add_library(vst3_pluginterfaces STATIC
    ${vst3pi_SOURCE_DIR}/base/funknown.cpp
    ${vst3pi_SOURCE_DIR}/base/coreiids.cpp
    ${vst3pi_SOURCE_DIR}/base/conststringtable.cpp
    ${vst3pi_SOURCE_DIR}/base/ustring.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vst3iids.cpp)
target_include_directories(vst3_pluginterfaces PUBLIC ${CMAKE_BINARY_DIR}/_deps/vst3sdk)
if (MSVC)
    target_compile_options(vst3_pluginterfaces PRIVATE /W0)
endif()
