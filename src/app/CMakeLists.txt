add_executable(winregedit_app winmain.cpp)
target_link_libraries(winregedit_app PRIVATE gui core threads)
