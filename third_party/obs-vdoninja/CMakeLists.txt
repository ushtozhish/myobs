cmake_minimum_required(VERSION 3.28...3.30)

find_package(CURL REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale swresample)
find_package(Qt6 REQUIRED Widgets)
find_package(obs-frontend-api REQUIRED)
find_package(Threads REQUIRED)

# VDO.Ninja requires WebSocket support and two small relay/TCP fixes that are
# not present in the shared libdatachannel package used by other OBS modules.
# Build the pinned, patched source as a private static dependency so the
# transport is self-contained and does not replace OBS's global package.
# MyOBS: libdatachannel v0.20.2 + third_party/patches/libdatachannel-0.20.2-myobs.patch,
# prepared by ci/build-app.ps1 and passed as MYOBS_LIBDATACHANNEL_DIR.
set(VDONINJA_LIBDATACHANNEL_SOURCE "${MYOBS_LIBDATACHANNEL_DIR}")
if(NOT EXISTS "${VDONINJA_LIBDATACHANNEL_SOURCE}/CMakeLists.txt")
  message(
    FATAL_ERROR
      "Pinned VDO.Ninja libdatachannel source is missing: ${VDONINJA_LIBDATACHANNEL_SOURCE}"
  )
endif()

# --- Private Mbed TLS for the VDO.Ninja transport -------------------------
# obs-deps ships Mbed TLS 3.6.4. Its DTLS server rejects the fragmented DTLS 1.2
# ClientHello that Chromium-based browsers send ("SSL - The requested feature
# is not available"), so calls never deliver media. Build a newer 3.6 LTS as a
# private static library with prefixed target names and use it only for
# libdatachannel/libSRTP inside this module. The rest of OBS keeps obs-deps.
# MyOBS: official mbedtls-3.6.7 release (SHA-256 checked) from ci/build-app.ps1 (MYOBS_MBEDTLS_DIR).
set(VDONINJA_MBEDTLS_SOURCE "${MYOBS_MBEDTLS_DIR}")
if(NOT EXISTS "${VDONINJA_MBEDTLS_SOURCE}/CMakeLists.txt")
  message(
    FATAL_ERROR
      "Private Mbed TLS source for VDO.Ninja is missing: ${VDONINJA_MBEDTLS_SOURCE}\n"
      "Download the official mbedtls-3.6.7.tar.bz2 release archive and extract it into .deps"
  )
endif()
file(
  STRINGS "${VDONINJA_MBEDTLS_SOURCE}/include/mbedtls/build_info.h" _vdoninja_mbedtls_version
  REGEX "#define[ \t]+MBEDTLS_VERSION_STRING[ \t]+\""
)
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" VDONINJA_MBEDTLS_VERSION "${_vdoninja_mbedtls_version}")
if(NOT VDONINJA_MBEDTLS_VERSION OR VDONINJA_MBEDTLS_VERSION VERSION_LESS 3.6.6)
  message(FATAL_ERROR "VDO.Ninja needs Mbed TLS 3.6.6 or newer, found '${VDONINJA_MBEDTLS_VERSION}'")
endif()
message(STATUS "obs-vdoninja: private static Mbed TLS ${VDONINJA_MBEDTLS_VERSION}")

set(MBEDTLS_TARGET_PREFIX "vdoninja_" CACHE STRING "Prefix for private Mbed TLS targets" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Build private static Mbed TLS" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "No shared Mbed TLS" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Do not build Mbed TLS programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Do not build Mbed TLS tests" FORCE)
set(GEN_FILES OFF CACHE BOOL "Use pre-generated Mbed TLS sources from the release archive" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Do not promote Mbed TLS warnings" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "Do not install private Mbed TLS" FORCE)

add_subdirectory(
  "${VDONINJA_MBEDTLS_SOURCE}"
  "${CMAKE_CURRENT_BINARY_DIR}/mbedtls-vdoninja"
  EXCLUDE_FROM_ALL
)

foreach(mbedtls_target IN ITEMS mbedcrypto mbedx509 mbedtls everest p256m)
  foreach(candidate IN ITEMS vdoninja_${mbedtls_target} ${mbedtls_target})
    if(TARGET ${candidate})
      target_compile_options(${candidate} PRIVATE /MP1 /Z7 /WX-)
      set_property(TARGET ${candidate} PROPERTY COMPILE_WARNING_AS_ERROR OFF)
    endif()
  endforeach()
endforeach()

if(NOT TARGET vdoninja_mbedtls OR NOT TARGET vdoninja_mbedcrypto)
  message(FATAL_ERROR "Private Mbed TLS did not create the expected vdoninja_* targets")
endif()

# libdatachannel uses the DTLS-SRTP extension, which is off in the default
# Mbed TLS configuration (obs-deps enables it the same way). PUBLIC on the
# crypto library propagates the define to x509, tls and every consumer.
target_compile_definitions(vdoninja_mbedcrypto PUBLIC MBEDTLS_SSL_DTLS_SRTP)

if(TARGET MbedTLS::MbedTLS)
  message(FATAL_ERROR "MbedTLS::MbedTLS already exists; the private VDO.Ninja Mbed TLS cannot be wired")
endif()
add_library(MbedTLS::MbedTLS ALIAS vdoninja_mbedtls)
# Consumed by the bundled libSRTP when ENABLE_MBEDTLS is on.
set(MBEDTLS_INCLUDE_DIRS "${VDONINJA_MBEDTLS_SOURCE}/include")
# BUILD_INTERFACE keeps the private target out of libSRTP's install(EXPORT) set.
set(MBEDTLS_LIBRARIES "$<BUILD_INTERFACE:vdoninja_mbedcrypto>")
# ---------------------------------------------------------------------------

set(USE_MBEDTLS ON CACHE BOOL "Use MbedTLS for the VDO.Ninja transport" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "Keep WebSocket signaling enabled" FORCE)
set(NO_MEDIA OFF CACHE BOOL "Keep WebRTC media transport enabled" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "Do not build libdatachannel examples" FORCE)
set(NO_TESTS ON CACHE BOOL "Do not build libdatachannel tests" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "Do not promote dependency warnings" FORCE)

# libdatachannel 0.20.2 vendors plog with a pre-CMake-3.5 declaration. CMake
# 4.x needs an explicit policy floor to configure that preserved dependency.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(
  "${VDONINJA_LIBDATACHANNEL_SOURCE}"
  "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel-vdoninja"
  EXCLUDE_FROM_ALL
)

# Only the static libdatachannel is linked into obs-vdoninja. Keep the unused
# shared variant out of the default build (it trips OBS warnings-as-errors).
if(TARGET datachannel)
  set_target_properties(datachannel PROPERTIES EXCLUDE_FROM_ALL ON EXCLUDE_FROM_DEFAULT_BUILD ON)
endif()

# VS 2026 can report C1902 when these legacy projects combine program-database
# debug information with compiler-level parallelism. Keep debug information in
# object files and compile each target serially, matching the module workaround.
foreach(
  dependency_target
  IN ITEMS datachannel datachannel-static juice-static srtp2 usrsctp
)
  if(TARGET ${dependency_target})
    target_compile_options(${dependency_target} PRIVATE /MP1 /Z7 /WX-)
    set_property(TARGET ${dependency_target} PROPERTY COMPILE_WARNING_AS_ERROR OFF)
  endif()
endforeach()

add_library(obs-vdoninja MODULE)
add_library(OBS::obs-vdoninja ALIAS obs-vdoninja)

target_sources(
  obs-vdoninja
  PRIVATE
    src/plugin-main.cpp
    src/plugin-main.h
    src/vdoninja-alpha-sync.cpp
    src/vdoninja-alpha-sync.h
    src/vdoninja-audio-red.cpp
    src/vdoninja-audio-red.h
    src/vdoninja-auto-inbound-state.h
    src/vdoninja-auto-scene-manager.cpp
    src/vdoninja-auto-scene-manager.h
    src/vdoninja-bitrate-controller.cpp
    src/vdoninja-bitrate-controller.h
    src/vdoninja-common.h
    src/vdoninja-data-channel.cpp
    src/vdoninja-data-channel.h
    src/vdoninja-dock.cpp
    src/vdoninja-dock.h
    src/vdoninja-guest-return.cpp
    src/vdoninja-guest-return.h
    src/vdoninja-guest-return-widget.cpp
    src/vdoninja-guest-return-widget.h
    src/vdoninja-h264-profile.cpp
    src/vdoninja-h264-profile.h
    src/vdoninja-ice-candidate-queue.cpp
    src/vdoninja-ice-candidate-queue.h
    src/vdoninja-layout.cpp
    src/vdoninja-layout.h
    src/vdoninja-loss-protection.cpp
    src/vdoninja-loss-protection.h
    src/vdoninja-module-lifecycle.cpp
    src/vdoninja-module-lifecycle.h
    src/vdoninja-network-settings.cpp
    src/vdoninja-network-settings.h
    src/vdoninja-output.cpp
    src/vdoninja-output.h
    src/vdoninja-peer-manager.cpp
    src/vdoninja-peer-manager.h
    src/vdoninja-publish-obs.h
    src/vdoninja-publish-settings.cpp
    src/vdoninja-publish-settings.h
    src/vdoninja-reliability.cpp
    src/vdoninja-reliability.h
    src/vdoninja-rtcp-feedback.cpp
    src/vdoninja-rtcp-feedback.h
    src/vdoninja-rtp-pacer.cpp
    src/vdoninja-rtp-pacer.h
    src/vdoninja-rtp-repair.cpp
    src/vdoninja-rtp-repair.h
    src/vdoninja-rtp-utils.cpp
    src/vdoninja-rtp-utils.h
    src/vdoninja-signaling.cpp
    src/vdoninja-signaling.h
    src/vdoninja-signaling-protocol.cpp
    src/vdoninja-signaling-protocol.h
    src/vdoninja-source.cpp
    src/vdoninja-source.h
    src/vdoninja-system-cpu.cpp
    src/vdoninja-system-cpu.h
    src/vdoninja-track-utils.h
    src/vdoninja-update-checker.cpp
    src/vdoninja-update-checker.h
    src/vdoninja-utils.cpp
    src/vdoninja-utils.h
    src/vdoninja-video-color.h
    src/vdoninja-video-keyframe-gate.h
    src/vdoninja-whep-host.cpp
    src/vdoninja-whep-host.h
)

target_compile_features(obs-vdoninja PRIVATE cxx_std_20)
target_compile_definitions(
  obs-vdoninja
  PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN VDONINJA_MBEDTLS_VERSION="${VDONINJA_MBEDTLS_VERSION}"
)
target_compile_options(obs-vdoninja PRIVATE /W4 /MP1 /Z7)

target_include_directories(
  obs-vdoninja
  BEFORE
  PRIVATE
    "${VDONINJA_LIBDATACHANNEL_SOURCE}/include"
    src
)

target_link_libraries(
  obs-vdoninja
  PRIVATE
    OBS::obs-frontend-api
    OBS::libobs
    CURL::libcurl
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swresample
    FFmpeg::swscale
    VdoNinjaDataChannel::Static
    Qt::Widgets
    Threads::Threads
    bcrypt
    crypt32
    ws2_32
)

set_target_properties(
  obs-vdoninja
  PROPERTIES COMPILE_WARNING_AS_ERROR OFF
             FOLDER plugins
             PREFIX ""
             AUTOMOC ON
             NO_SYSTEM_FROM_IMPORTED ON
)
# MyOBS layout: obs-plugins/64bit/obs-vdoninja.dll, data/obs-plugins/obs-vdoninja
install(TARGETS obs-vdoninja LIBRARY DESTINATION ${MYOBS_PLUGIN_DIR} RUNTIME DESTINATION ${MYOBS_PLUGIN_DIR})
install(DIRECTORY data/ DESTINATION data/obs-plugins/obs-vdoninja)
