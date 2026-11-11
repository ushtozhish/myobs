# «NDI» input. Needs the NDI SDK headers (fetched by ci/build-app.ps1 into engine-build/ndi-sdk);
# the NDI Runtime itself is loaded at run time from the user's machine.
find_path(NDI_SDK_INCLUDE_DIR Processing.NDI.Lib.h PATHS "${MYOBS_NDI_SDK_DIR}" NO_DEFAULT_PATH)
if(NOT NDI_SDK_INCLUDE_DIR)
  message(WARNING "NDI SDK headers not found (MYOBS_NDI_SDK_DIR) - NDI input is not built")
  return()
endif()
add_library(myobs-ndi MODULE myobs-ndi.cpp)
set_target_properties(myobs-ndi PROPERTIES PREFIX "")
target_include_directories(myobs-ndi PRIVATE "${NDI_SDK_INCLUDE_DIR}")
target_link_libraries(myobs-ndi PRIVATE OBS::libobs)
install(TARGETS myobs-ndi LIBRARY DESTINATION ${MYOBS_PLUGIN_DIR} RUNTIME DESTINATION ${MYOBS_PLUGIN_DIR})
