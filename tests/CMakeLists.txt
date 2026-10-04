find_package(Qt6 REQUIRED COMPONENTS Test)

function(zterminal_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} Qt6::Test)
  target_compile_definitions(${name} PRIVATE ZTERMINAL_EXPECTED_VERSION="${PROJECT_VERSION}")
  zterminal_set_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-${name}"
    TIMEOUT 60)
endfunction()

zterminal_add_test(tst_terminal zterminal_core)
zterminal_add_test(tst_selection zterminal_core)
zterminal_add_test(tst_pty zterminal_core)
zterminal_add_test(tst_misc zterminal_core)
zterminal_add_test(tst_view zterminal_ui)

# The binary and the zt launcher answer --version/--help without a display.
add_test(NAME zterminal_version COMMAND zterminal --version)
set_tests_properties(zterminal_version PROPERTIES
  PASS_REGULAR_EXPRESSION "^zterminal ${PROJECT_VERSION}\n$")
add_test(NAME zt_version COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt --version)
set_tests_properties(zt_version PROPERTIES
  ENVIRONMENT "ZTERMINAL_BIN=$<TARGET_FILE:zterminal>"
  PASS_REGULAR_EXPRESSION "^zterminal ${PROJECT_VERSION}\n$")
add_test(NAME zt_help COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt --help)
set_tests_properties(zt_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: zt ")
add_test(NAME zterminal_bad_option COMMAND zterminal --bogus)
set_tests_properties(zterminal_bad_option PROPERTIES WILL_FAIL ON
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
