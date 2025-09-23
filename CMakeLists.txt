cmake_minimum_required(VERSION 3.20)
project(SP_COURSE_WORK VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

option(USE_WIN32_THREADPOOL "Use Windows native thread pool adapter" ON)

add_subdirectory(src/core)
add_subdirectory(src/threads)
add_subdirectory(src/gui)
add_subdirectory(src/app)
add_subdirectory(src/helper)
add_subdirectory(src/tests)
