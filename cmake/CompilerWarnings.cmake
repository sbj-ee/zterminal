# Warning flags for zterminal's own targets (not third_party).
function(zterminal_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference)
endfunction()
