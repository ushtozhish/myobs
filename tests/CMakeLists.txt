add_executable(myobs-tests myobs-tests.cpp)
target_link_libraries(myobs-tests PRIVATE myobs-core OBS::libobs FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
install(TARGETS myobs-tests RUNTIME DESTINATION ${MYOBS_BIN_DIR})

# clock-drift buffer between the engine and audio devices (no engine needed)
add_executable(myobs-drift-test drift-test.cpp)
target_link_libraries(myobs-drift-test PRIVATE myobs-core)
install(TARGETS myobs-drift-test RUNTIME DESTINATION ${MYOBS_BIN_DIR})
