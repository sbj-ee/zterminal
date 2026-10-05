# zterminal_ui: widgets shared by the app and the GUI tests.
add_library(zterminal_ui STATIC
  TerminalView.cpp TerminalView.hpp
  MainWindow.cpp MainWindow.hpp
  FindBar.cpp FindBar.hpp
  SessionWidget.cpp SessionWidget.hpp
  PreferencesDialog.cpp PreferencesDialog.hpp
  AppSettings.cpp AppSettings.hpp
  SessionDialog.cpp SessionDialog.hpp
  VaultManager.cpp VaultManager.hpp
  VaultDialogs.cpp VaultDialogs.hpp
  PasteConfirmDialog.cpp PasteConfirmDialog.hpp
  UpdateDialog.cpp UpdateDialog.hpp
  UpdateManager.cpp UpdateManager.hpp
)
target_include_directories(zterminal_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zterminal_ui PUBLIC zterminal_core Qt6::Gui Qt6::Widgets)
zterminal_set_warnings(zterminal_ui)

add_executable(zterminal main.cpp)
target_link_libraries(zterminal PRIVATE zterminal_ui)
zterminal_set_warnings(zterminal)
# The askpass helper must be built whenever the app is (it's found next to it).
add_dependencies(zterminal zterminal-askpass)

# macOS: copy askpass + icns into the build-tree .app (TARGET POST_BUILD must
# live in this directory — the same CMakeLists that created zterminal).
# zterminal.icns is produced by target zterminal-icns (cmake/MacBundle.cmake).
if(APPLE)
  add_custom_command(TARGET zterminal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:zterminal>/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:zterminal-askpass>"
            "$<TARGET_BUNDLE_CONTENT_DIR:zterminal>/MacOS/zterminal-askpass"
    COMMENT "Bundling zterminal-askpass into zterminal.app"
    VERBATIM)
  add_custom_command(TARGET zterminal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:zterminal>/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/zterminal.icns"
            "$<TARGET_BUNDLE_CONTENT_DIR:zterminal>/Resources/zterminal.icns"
    COMMENT "Copying zterminal.icns into zterminal.app"
    VERBATIM)
endif()
