add_executable(myobs WIN32 main.cpp gpu.hpp display.hpp inputs.hpp thumbs.hpp demo.hpp props.hpp)
target_link_libraries(myobs PRIVATE myobs-core OBS::libobs Qt6::Widgets)
install(TARGETS myobs RUNTIME DESTINATION ${MYOBS_BIN_DIR})
