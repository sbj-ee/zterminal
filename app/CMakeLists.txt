# zterminal_ui: widgets shared by the app and the GUI tests.
add_library(zterminal_ui STATIC
  TerminalView.cpp TerminalView.hpp
  MainWindow.cpp MainWindow.hpp
  PreferencesDialog.cpp PreferencesDialog.hpp
  AppSettings.cpp AppSettings.hpp
)
target_include_directories(zterminal_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zterminal_ui PUBLIC zterminal_core Qt6::Gui Qt6::Widgets)
zterminal_set_warnings(zterminal_ui)

add_executable(zterminal main.cpp)
target_link_libraries(zterminal PRIVATE zterminal_ui)
zterminal_set_warnings(zterminal)
