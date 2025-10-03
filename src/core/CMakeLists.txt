add_library(core STATIC
        registry/RegistryFacade.cpp
        watcher/RegistryWatcher.cpp
        snapshot/SnapshotManager.cpp
        logging/Logger.cpp
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core PRIVATE advapi32)
target_compile_definitions(core PRIVATE WINDOWS_LEAN_AND_MEAN UNICODE)
