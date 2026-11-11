cmake_minimum_required(VERSION 3.28)

project(myobs VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# Engine: libobs (OBS core) installed by ci/build-engine.ps1; FFmpeg and Qt from the OBS prebuilt deps.
find_package(libobs REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)

# Same folder layout as OBS, so libobs finds its data and plugins next to the program.
set(MYOBS_BIN_DIR bin/64bit)
set(MYOBS_PLUGIN_DIR obs-plugins/64bit)

add_subdirectory(core)
add_subdirectory(plugins/myobs-clip)
add_subdirectory(plugins/myobs-ndi)

# Calls (VDO.Ninja, GPL-3.0): built when the patched libdatachannel and Mbed TLS are prepared.
if(MYOBS_LIBDATACHANNEL_DIR AND MYOBS_MBEDTLS_DIR)
  add_subdirectory(third_party/obs-vdoninja)
else()
  message(WARNING "MYOBS_LIBDATACHANNEL_DIR / MYOBS_MBEDTLS_DIR not set - calls (obs-vdoninja) are not built")
endif()
add_subdirectory(app)
add_subdirectory(tests)
