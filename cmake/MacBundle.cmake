# macOS: build zterminal as a real zterminal.app bundle (Apple Silicon only).
# Included from the top-level CMakeLists.txt after the zterminal target exists.
#   - Info.plist from cmake/Info.plist.in (versions from PROJECT_VERSION)
#   - zterminal.icns generated from assets/icons/*.png with iconutil
#   - zterminal-askpass copied into Contents/MacOS (SSH_ASKPASS helper)
# Qt frameworks/plugins and libsodium are copied at install time by
# macdeployqt (cmake/MacDeploy.cmake.in, run by `cpack -G DragNDrop`).

set(ZTERMINAL_BUNDLE_ID "ee.sbj.zterminal")

set_target_properties(zterminal PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${ZTERMINAL_BUNDLE_ID}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  OUTPUT_NAME "zterminal"
)

# --- App icon: iconset from PNG art -> zterminal.icns --------------------
find_program(ZTERMINAL_ICONUTIL iconutil REQUIRED)
set(_icons "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons")
set(_iconset "${CMAKE_CURRENT_BINARY_DIR}/zterminal.iconset")
set(_icns "${CMAKE_CURRENT_BINARY_DIR}/zterminal.icns")
set(_iconmap
  icon_16x16:16 icon_16x16@2x:32 icon_32x32:32 icon_32x32@2x:64
  icon_128x128:128 icon_128x128@2x:256 icon_256x256:256 icon_256x256@2x:512
  icon_512x512:512)
set(_copy_cmds)
set(_icon_deps)
foreach(_pair IN LISTS _iconmap)
  string(REPLACE ":" ";" _kv "${_pair}")
  list(GET _kv 0 _name)
  list(GET _kv 1 _size)
  list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy
       "${_icons}/zterminal-${_size}.png" "${_iconset}/${_name}.png")
  list(APPEND _icon_deps "${_icons}/zterminal-${_size}.png")
endforeach()
list(REMOVE_DUPLICATES _icon_deps)
add_custom_command(
  OUTPUT "${_icns}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_iconset}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset}"
  ${_copy_cmds}
  COMMAND "${ZTERMINAL_ICONUTIL}" -c icns "${_iconset}" -o "${_icns}"
  DEPENDS ${_icon_deps}
  COMMENT "Generating zterminal.icns"
  VERBATIM)
target_sources(zterminal PRIVATE "${_icns}")
set_source_files_properties("${_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

# --- Deployment (runs at install time, i.e. inside `cpack -G DragNDrop`) ---
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qtbin "${_qmake}" DIRECTORY)
find_program(ZTERMINAL_MACDEPLOYQT NAMES macdeployqt macdeployqt6
  HINTS "${_qtbin}" REQUIRED)
# libsodium is not a Qt dependency; pass its brew libdir to macdeployqt -libpath.
set(ZTERMINAL_SODIUM_LIBDIR "")
if(SODIUM_LIBRARY_DIRS)
  list(GET SODIUM_LIBRARY_DIRS 0 ZTERMINAL_SODIUM_LIBDIR)
elseif(DEFINED ENV{HOMEBREW_PREFIX} AND EXISTS "$ENV{HOMEBREW_PREFIX}/lib")
  set(ZTERMINAL_SODIUM_LIBDIR "$ENV{HOMEBREW_PREFIX}/opt/libsodium/lib")
endif()
# Fallback: ask brew.
if(ZTERMINAL_SODIUM_LIBDIR STREQUAL "" OR NOT EXISTS "${ZTERMINAL_SODIUM_LIBDIR}")
  find_program(_ZTERMINAL_BREW brew)
  if(_ZTERMINAL_BREW)
    execute_process(
      COMMAND "${_ZTERMINAL_BREW}" --prefix libsodium
      OUTPUT_VARIABLE _sodium_prefix
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_sodium_prefix AND EXISTS "${_sodium_prefix}/lib")
      set(ZTERMINAL_SODIUM_LIBDIR "${_sodium_prefix}/lib")
    endif()
  endif()
endif()
set(ZTERMINAL_CHECK_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos/check-bundle.sh")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacDeploy.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/MacDeploy.cmake" @ONLY)
message(STATUS "zterminal: macOS app bundle ${ZTERMINAL_BUNDLE_ID}, macdeployqt ${ZTERMINAL_MACDEPLOYQT}, sodium ${ZTERMINAL_SODIUM_LIBDIR}")
