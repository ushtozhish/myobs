add_library(myobs-clip MODULE myobs-clip.cpp)
target_link_libraries(myobs-clip PRIVATE OBS::libobs FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale
                                         FFmpeg::swresample)
set_target_properties(myobs-clip PROPERTIES PREFIX "")
install(TARGETS myobs-clip LIBRARY DESTINATION ${MYOBS_PLUGIN_DIR} RUNTIME DESTINATION ${MYOBS_PLUGIN_DIR})
