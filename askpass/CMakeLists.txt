# zterminal-askpass: plain C, no Qt, so ssh's helper starts instantly and
# links nothing that could log. Built next to the zterminal binary so a build
# tree finds it; installed to <prefix>/libexec/zterminal/.
# The prompt policy is its own library so the tests and the fuzzer use the
# exact code the helper runs.
add_library(zterminal_askpass_policy STATIC askpass_policy.c askpass_policy.h)
set_target_properties(zterminal_askpass_policy PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_include_directories(zterminal_askpass_policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(zterminal_askpass_policy PRIVATE -Wall -Wextra)

add_executable(zterminal-askpass zterminal-askpass.c)
target_link_libraries(zterminal-askpass PRIVATE zterminal_askpass_policy)
set_target_properties(zterminal-askpass PROPERTIES
  C_STANDARD 11
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/app)
target_compile_options(zterminal-askpass PRIVATE -Wall -Wextra)
