# Compiler and linker hardening for every target (zterminal, the askpass
# helper, libvterm, tests). On by default; -DZTERMINAL_HARDENING=OFF to skip.
#   - PIE executables (ASLR for the main binary too)
#   - -fstack-protector-strong; -fstack-clash-protection and -fcf-protection
#     where the compiler supports them
#   - _FORTIFY_SOURCE=3 (Linux) / 2 (macOS) in optimised builds (it needs -O)
#   - Linux: full RELRO + immediate binding (-z relro -z now), non-executable stack
# macOS hardened runtime is applied when the bundle is signed (MacDeploy.cmake.in).
option(ZTERMINAL_HARDENING "Build with compiler/linker hardening flags" ON)

if(NOT ZTERMINAL_HARDENING OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  return()
endif()

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)

check_pie_supported(LANGUAGES C CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-fstack-protector-strong)

check_cxx_compiler_flag(-fstack-clash-protection ZT_HAVE_STACK_CLASH)
if(ZT_HAVE_STACK_CLASH AND NOT APPLE)
  add_compile_options(-fstack-clash-protection)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  check_cxx_compiler_flag(-fcf-protection=full ZT_HAVE_CF_PROTECTION)
  if(ZT_HAVE_CF_PROTECTION)
    add_compile_options(-fcf-protection=full)
  endif()
endif()

# FORTIFY needs optimisation and conflicts with sanitizers; undefine first so
# a distro default (Ubuntu's gcc sets _FORTIFY_SOURCE=3) doesn't warn.
if(NOT ZTERMINAL_SANITIZE AND NOT ZTERMINAL_FUZZ)
  if(APPLE)
    set(_zt_fortify 2)
  else()
    set(_zt_fortify 3)
  endif()
  add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=${_zt_fortify}>")
endif()

if(NOT APPLE)
  check_linker_flag(CXX "LINKER:-z,relro,-z,now" ZT_HAVE_RELRO_NOW)
  if(ZT_HAVE_RELRO_NOW)
    add_link_options("LINKER:-z,relro,-z,now")
  endif()
  check_linker_flag(CXX "LINKER:-z,noexecstack" ZT_HAVE_NOEXECSTACK)
  if(ZT_HAVE_NOEXECSTACK)
    add_link_options("LINKER:-z,noexecstack")
  endif()
endif()
