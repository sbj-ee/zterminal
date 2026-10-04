# zterminal_core: no widgets (Qt Gui only for the QRgb colour type). PTY, libvterm
# wrapper, scrollback, selection, settings.
add_library(zterminal_core STATIC
  Pty.cpp Pty.hpp
  Terminal.cpp Terminal.hpp
  Selection.cpp Selection.hpp
  ColorScheme.cpp ColorScheme.hpp
  MouseSettings.cpp MouseSettings.hpp
  WindowTitle.cpp WindowTitle.hpp
  CommandLine.cpp CommandLine.hpp
)
target_include_directories(zterminal_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${ZTERMINAL_GENERATED_DIR})
target_link_libraries(zterminal_core PUBLIC Qt6::Core Qt6::Gui vterm PRIVATE util)
zterminal_set_warnings(zterminal_core)
