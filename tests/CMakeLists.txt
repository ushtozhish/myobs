add_executable(myobs-tests myobs-tests.cpp)
target_link_libraries(myobs-tests PRIVATE myobs-core OBS::libobs)
install(TARGETS myobs-tests RUNTIME DESTINATION ${MYOBS_BIN_DIR})
