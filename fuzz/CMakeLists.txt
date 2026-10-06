# libFuzzer harnesses (clang only): cmake -DZTERMINAL_FUZZ=ON -DCMAKE_C_COMPILER=clang
# -DCMAKE_CXX_COMPILER=clang++ -DZTERMINAL_BUILD_TESTS=OFF. Every target in the
# tree is then built with -fsanitize=fuzzer-no-link,address,undefined
# (cmake/Sanitizers.cmake); each harness links -fsanitize=fuzzer.
# Run one: ./build-fuzz/fuzz/fuzz_terminal -max_total_time=60 fuzz/corpus/terminal
foreach(name vault session_import terminal paste_title update_json)
  add_executable(fuzz_${name} fuzz_${name}.cpp)
  target_link_libraries(fuzz_${name} PRIVATE zterminal_core)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
endforeach()
