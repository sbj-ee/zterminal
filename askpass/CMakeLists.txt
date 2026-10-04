# zterminal-askpass: plain C, no Qt, so ssh's helper starts instantly and
# links nothing that could log. Built next to the zterminal binary so a build
# tree finds it; installed to <prefix>/libexec/zterminal/.
add_executable(zterminal-askpass zterminal-askpass.c)
set_target_properties(zterminal-askpass PROPERTIES
  C_STANDARD 11
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)
target_compile_options(zterminal-askpass PRIVATE -Wall -Wextra)
