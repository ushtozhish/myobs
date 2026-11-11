# MyOBS: Mbed TLS for the calls transport is the private static build made by obs-vdoninja
# (third_party/obs-vdoninja/CMakeLists.txt). libdatachannel and libSRTP look for it with
# find_package(MbedTLS); this finder takes precedence over theirs and reports the private
# targets, keeping MBEDTLS_INCLUDE_DIRS / MBEDTLS_LIBRARIES as obs-vdoninja set them.
if(TARGET MbedTLS::MbedTLS)
  set(MbedTLS_FOUND TRUE)
  set(MBEDTLS_FOUND TRUE)
  set(MbedTLS_VERSION 3.6.7)
  set(MbedTLS_INCLUDE_DIR "${MBEDTLS_INCLUDE_DIRS}")
  set(MbedTLS_LIBRARY MbedTLS::MbedTLS)
  if(NOT TARGET MbedTLS::MbedCrypto AND TARGET vdoninja_mbedcrypto)
    add_library(MbedTLS::MbedCrypto ALIAS vdoninja_mbedcrypto)
  endif()
  if(NOT TARGET MbedTLS::MbedX509 AND TARGET vdoninja_mbedx509)
    add_library(MbedTLS::MbedX509 ALIAS vdoninja_mbedx509)
  endif()
  return()
endif()
set(MbedTLS_FOUND FALSE)
if(MbedTLS_FIND_REQUIRED)
  message(FATAL_ERROR "MbedTLS: only the private build of obs-vdoninja is used in MyOBS")
endif()
