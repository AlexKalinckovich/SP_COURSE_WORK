cmake_minimum_required(VERSION 3.20)
project(SP_COURSE_WORK VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Root of your source tree (project root)
set(SRC_ROOT ${CMAKE_SOURCE_DIR}/src)

# Core library (static)
add_library(core_lib STATIC
        ${SRC_ROOT}/threads/IThreadManager.h
        ${SRC_ROOT}/threads/StdThreadPool.h
        ${SRC_ROOT}/threads/StdThreadPool.cpp
        ${SRC_ROOT}/threads/WinThreadPoolAdapter.h
        ${SRC_ROOT}/threads/WinThreadPoolAdapter.cpp

        ${SRC_ROOT}/core/loggin/Logger.h
        ${SRC_ROOT}/core/loggin/Logger.cpp
        ${SRC_ROOT}/core/loggin/FileLogger.h
        ${SRC_ROOT}/core/loggin/FileLogger.cpp
        ${SRC_ROOT}/core/loggin/EventLogSink.h
        ${SRC_ROOT}/core/loggin/EventLogSink.cpp

        ${SRC_ROOT}/core/registry/RegistryHelpers.h
        ${SRC_ROOT}/core/registry/RegistryHelpers.cpp
        ${SRC_ROOT}/core/registry/RegistryFacade.h
        ${SRC_ROOT}/core/registry/RegistryFacade.cpp
        ${SRC_ROOT}/core/registry/RegistryKey.h
        ${SRC_ROOT}/core/registry/RegistryKey.cpp

        ${SRC_ROOT}/gui/RegistryTreeView.h
        ${SRC_ROOT}/gui/RegistryTreeView.cpp
        ${SRC_ROOT}/gui/MainWindow.h
        ${SRC_ROOT}/gui/MainWindow.cpp
        ${SRC_ROOT}/gui/Messages.h
)

target_include_directories(core_lib PUBLIC
        ${SRC_ROOT}
        ${SRC_ROOT}/core
        ${SRC_ROOT}/gui
        ${SRC_ROOT}/threads
)

# create executable
add_executable(${PROJECT_NAME}
        ${SRC_ROOT}/app/main.cpp
)

# link the core static lib
target_link_libraries(${PROJECT_NAME} PRIVATE
        core_lib
        comctl32
        advapi32
        user32
        gdi32
        shell32
        ole32
        oleaut32
        uuid
        comdlg32
)

# set subsystem: choose GUI (-subsystem,windows) only if you implement wWinMain
# Otherwise for debug consoles, omit this or use -mconsole
set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_compile_definitions(core_lib PUBLIC UNICODE _UNICODE)
target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE)


# Compiler flags
if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive-)
    target_compile_options(core_lib PRIVATE /W4 /permissive-)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(core_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()
