# Warning flags for zterminal's own targets (not third_party).
#   -DZTERMINAL_WERROR=ON   treat warnings as errors (CI sets this; off by
#                           default so a newer compiler doesn't break a build)
option(ZTERMINAL_WERROR "Treat compiler warnings in zterminal's own targets as errors" OFF)

function(zterminal_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference)
  if(ZTERMINAL_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# The C targets (askpass helper): no C++-only flags.
function(zterminal_set_c_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra)
  if(ZTERMINAL_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
