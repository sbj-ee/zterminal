# libvterm 0.3.3 (MIT), bundled unmodified. See README.zterminal.md.
add_library(vterm STATIC
  src/encoding.c
  src/keyboard.c
  src/mouse.c
  src/parser.c
  src/pen.c
  src/screen.c
  src/state.c
  src/unicode.c
  src/vterm.c
)
set_target_properties(vterm PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(vterm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Third-party code: do not apply zterminal's warning flags.
target_compile_options(vterm PRIVATE -w)
