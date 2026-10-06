# Sanitizer and fuzzing builds (CI jobs "Sanitizers" and "Fuzz").
#   -DZTERMINAL_SANITIZE=address,undefined   instrument everything; run ctest
#   -DZTERMINAL_FUZZ=ON (clang)              libFuzzer harnesses in fuzz/
set(ZTERMINAL_SANITIZE "" CACHE STRING "Comma-separated -fsanitize= list (e.g. address,undefined)")
option(ZTERMINAL_FUZZ "Build the libFuzzer harnesses in fuzz/ (clang only)" OFF)

if(ZTERMINAL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ZTERMINAL_FUZZ needs clang (libFuzzer): -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
  endif()
  set(_zt_san "fuzzer-no-link,address,undefined")
  if(ZTERMINAL_SANITIZE)
    set(_zt_san "fuzzer-no-link,${ZTERMINAL_SANITIZE}")
  endif()
elseif(ZTERMINAL_SANITIZE)
  set(_zt_san "${ZTERMINAL_SANITIZE}")
endif()

if(_zt_san)
  add_compile_options(-fsanitize=${_zt_san} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  string(REPLACE "fuzzer-no-link," "" _zt_san_link "${_zt_san}")
  add_link_options(-fsanitize=${_zt_san_link})
  message(STATUS "zterminal: sanitizers ${_zt_san}")
endif()
