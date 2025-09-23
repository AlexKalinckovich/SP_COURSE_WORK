add_library(gui STATIC
        MainWindow.cpp
        KeyTreeView.cpp
        KeyEditor.cpp
        resources.rc
)

target_include_directories(gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gui PRIVATE core user32 gdi32 comctl32 comdlg32)
