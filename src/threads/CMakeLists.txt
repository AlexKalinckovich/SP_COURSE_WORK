cmake_minimum_required(VERSION 3.20)
project(SP_COURSE_WORK VERSION 0.1 LANGUAGES CXX)

add_library(threads STATIC
        IThreadManager.h
        StdThreadPool.h
        StdThreadPool.cpp
        IThreadManager.h
        StdThreadPool.h
        StdThreadPool.cpp
        WinThreadPoolAdapter.h
        WinThreadPoolAdapter.cpp
        ../core/loggin/Logger.h
        ../core/loggin/Logger.cpp
        ../core/loggin/FileLogger.h
        ../core/loggin/FileLogger.cpp
        ../core/loggin/EventLogSink.h
        ../core/loggin/EventLogSink.cpp
        ../core/registry/RegistryHelpers.h
)

target_include_directories(threads PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(threads PUBLIC cxx_std_20)

if (MSVC)
    target_compile_options(threads PRIVATE /W4 /permissive-)
else()
    target_compile_options(threads PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
