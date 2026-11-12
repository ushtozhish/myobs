# MyOBS core: switcher and other engine-side logic shared by the program and the automatic tests.
add_library(myobs-core STATIC switcher.cpp switcher.hpp plugins.cpp plugins.hpp audiobus.cpp audiobus.hpp program.cpp program.hpp devices.cpp devices.hpp drift.hpp screenfeeds.hpp triggers.hpp)
target_include_directories(myobs-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(myobs-core PUBLIC OBS::libobs)
if(WIN32)
  target_link_libraries(myobs-core PUBLIC ole32)
endif()
