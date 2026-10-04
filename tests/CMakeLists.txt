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
zterminal_add_test(tst_settings zterminal_ui)
zterminal_add_test(tst_sessions zterminal_core)
zterminal_add_test(tst_sessionui zterminal_ui)
zterminal_add_test(tst_serial zterminal_core)
zterminal_add_test(tst_serialui zterminal_ui)
zterminal_add_test(tst_vault zterminal_core)
zterminal_add_test(tst_askpass zterminal_core)
target_compile_definitions(tst_askpass PRIVATE ZTERMINAL_ASKPASS_BIN="$<TARGET_FILE:zterminal-askpass>")
add_dependencies(tst_askpass zterminal-askpass)
zterminal_add_test(tst_vaultui zterminal_ui)
zterminal_add_test(tst_sessionlog zterminal_core)
zterminal_add_test(tst_logui zterminal_ui)
add_dependencies(tst_logui zterminal-askpass)
set_tests_properties(tst_logui PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-tst_logui;ZTERMINAL_ASKPASS=$<TARGET_FILE:zterminal-askpass>"
  TIMEOUT 120)
add_dependencies(tst_vaultui zterminal-askpass)
set_tests_properties(tst_vaultui PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-tst_vaultui;ZTERMINAL_ASKPASS=$<TARGET_FILE:zterminal-askpass>"
  TIMEOUT 120)

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
# Saved sessions from the command line, against a read-only fixture config
# (tests/data/config/zterminal/sessions: "core-sw1" (ssh) and "lab box" (local)).
set(ZT_FIXTURE_ENV "XDG_CONFIG_HOME=${CMAKE_CURRENT_SOURCE_DIR}/data/config")
add_test(NAME zterminal_list_sessions COMMAND zterminal --list-sessions)
set_tests_properties(zterminal_list_sessions PROPERTIES ENVIRONMENT "${ZT_FIXTURE_ENV}"
  PASS_REGULAR_EXPRESSION "^core-sw1\nlab box\n$")
add_test(NAME zterminal_check_session_ok COMMAND zterminal --check-session "lab box")
set_tests_properties(zterminal_check_session_ok PROPERTIES ENVIRONMENT "${ZT_FIXTURE_ENV}")
add_test(NAME zterminal_unknown_session_message COMMAND zterminal --check-session nope)
set_tests_properties(zterminal_unknown_session_message PROPERTIES ENVIRONMENT "${ZT_FIXTURE_ENV}"
  PASS_REGULAR_EXPRESSION "unknown session \"nope\"; saved sessions: core-sw1, lab box")
add_test(NAME zterminal_unknown_session_fails COMMAND zterminal nope)
set_tests_properties(zterminal_unknown_session_fails PROPERTIES WILL_FAIL ON
  ENVIRONMENT "${ZT_FIXTURE_ENV};QT_QPA_PLATFORM=offscreen")
add_test(NAME zt_unknown_session_message COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt nope)
set_tests_properties(zt_unknown_session_message PROPERTIES
  ENVIRONMENT "${ZT_FIXTURE_ENV};ZTERMINAL_BIN=$<TARGET_FILE:zterminal>"
  PASS_REGULAR_EXPRESSION "zterminal: unknown session \"nope\"; saved sessions: core-sw1, lab box")
add_test(NAME zt_unknown_session_fails COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt nope)
set_tests_properties(zt_unknown_session_fails PROPERTIES WILL_FAIL ON
  ENVIRONMENT "${ZT_FIXTURE_ENV};ZTERMINAL_BIN=$<TARGET_FILE:zterminal>")
add_test(NAME zt_list_sessions COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt --list-sessions)
set_tests_properties(zt_list_sessions PROPERTIES
  ENVIRONMENT "${ZT_FIXTURE_ENV};ZTERMINAL_BIN=$<TARGET_FILE:zterminal>"
  PASS_REGULAR_EXPRESSION "^core-sw1\nlab box\n$")
add_test(NAME zt_serial_needs_device COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt serial)
set_tests_properties(zt_serial_needs_device PROPERTIES
  ENVIRONMENT "ZTERMINAL_BIN=$<TARGET_FILE:zterminal>"
  PASS_REGULAR_EXPRESSION "usage: zt serial <device> \\[baud\\]")
add_test(NAME zt_serial_bad_baud COMMAND ${CMAKE_SOURCE_DIR}/packaging/zt serial /dev/ttyUSB0 fast)
set_tests_properties(zt_serial_bad_baud PROPERTIES WILL_FAIL ON
  ENVIRONMENT "ZTERMINAL_BIN=$<TARGET_FILE:zterminal>")
add_test(NAME zterminal_check_serial_ok COMMAND zterminal --check serial /dev/ttyUSB0 115200)
add_test(NAME zterminal_bad_option COMMAND zterminal --bogus)
set_tests_properties(zterminal_bad_option PROPERTIES WILL_FAIL ON
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Global-menu regression test: needs an X server and a private session bus.
find_package(Qt6 COMPONENTS DBus QUIET)
find_program(ZTERMINAL_XVFB_RUN xvfb-run)
find_program(ZTERMINAL_DBUS_RUN_SESSION dbus-run-session)
if(TARGET Qt6::DBus AND ZTERMINAL_XVFB_RUN AND ZTERMINAL_DBUS_RUN_SESSION)
  add_executable(tst_globalmenu tst_globalmenu.cpp)
  target_link_libraries(tst_globalmenu PRIVATE zterminal_ui Qt6::DBus Qt6::Test)
  zterminal_set_warnings(tst_globalmenu)
  add_test(NAME tst_globalmenu
    COMMAND ${ZTERMINAL_DBUS_RUN_SESSION} -- ${ZTERMINAL_XVFB_RUN} -a $<TARGET_FILE:tst_globalmenu>)
  set_tests_properties(tst_globalmenu PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=xcb;QT_QPA_PLATFORMTHEME=;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config-tst_globalmenu"
    TIMEOUT 60)
else()
  message(STATUS "zterminal: tst_globalmenu skipped (needs Qt6::DBus, xvfb-run, dbus-run-session)")
endif()
