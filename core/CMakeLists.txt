# zterminal_core: no widgets (Qt Gui only for the QRgb colour type). PTY, libvterm
# wrapper, scrollback, selection, settings.
add_library(zterminal_core STATIC
  Pty.cpp Pty.hpp
  ProcessHardening.cpp ProcessHardening.hpp
  Terminal.cpp Terminal.hpp
  Selection.cpp Selection.hpp
  BrandThemes.h ColorScheme.cpp ColorScheme.hpp
  ThemeFile.cpp ThemeFile.h
  MouseSettings.cpp MouseSettings.hpp
  WindowTitle.cpp WindowTitle.hpp
  CommandLine.cpp CommandLine.hpp
  Session.cpp Session.hpp
  SessionStore.cpp SessionStore.hpp
  SessionExport.cpp SessionExport.hpp
  SerialBackend.cpp SerialBackend.hpp
  SecureBuffer.cpp SecureBuffer.hpp
  Vault.cpp Vault.hpp
  VaultBindings.cpp VaultBindings.hpp
  AskpassServer.cpp AskpassServer.hpp
  LogTextFilter.cpp LogTextFilter.hpp
  SessionLog.cpp SessionLog.hpp
  TerminalSearch.cpp TerminalSearch.hpp
  Reconnect.cpp Reconnect.hpp
  Scrollback.cpp Scrollback.hpp
  PasteGuard.cpp PasteGuard.hpp
  Update.cpp Update.hpp
  UpdateChecker.cpp UpdateChecker.hpp
)
target_include_directories(zterminal_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${ZTERMINAL_GENERATED_DIR})
target_link_libraries(zterminal_core PUBLIC Qt6::Core Qt6::Gui Qt6::SerialPort Qt6::Network vterm PkgConfig::SODIUM PRIVATE util)
zterminal_set_warnings(zterminal_core)

# Environment overrides for development only, e.g. ZTERMINAL_ASKPASS (askpass
# helper path). Release packages must be built without them, so a modified
# environment can't swap the helper that receives stored passwords. Off unless
# CMAKE_BUILD_TYPE is Debug; the tests don't need it (they use setters).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_zt_dev_overrides_default ON)
else()
  set(_zt_dev_overrides_default OFF)
endif()
option(ZTERMINAL_DEV_OVERRIDES "Honour development environment overrides such as ZTERMINAL_ASKPASS (debug builds only)"
       ${_zt_dev_overrides_default})
if(ZTERMINAL_DEV_OVERRIDES)
  target_compile_definitions(zterminal_core PUBLIC ZTERMINAL_DEV_OVERRIDES)
  message(STATUS "zterminal: ZTERMINAL_DEV_OVERRIDES=ON (environment overrides compiled in; not for release packages)")
endif()
