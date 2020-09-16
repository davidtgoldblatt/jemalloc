include(CheckCCompilerFlag)
include(CheckCSourceCompiles)

function(jemalloc_compilable header_str main_str result_var)
  check_c_source_compiles(
    "
    ${header_str}
    int main() {
      ${main_str}
      return 0;
    }"
    ${result_var})
endfunction()

# Takes warning flags as extra arguments.
function(jemalloc_compilable_nowarning header_str main_str result_var)
  check_c_compiler_flag(-Werror JEMALLOC_WERROR)
  check_c_compiler_flag(-herror_on_warning JEMALLOC_HERROR_ON_WARNING)
  if(JEMALLOC_WERROR)
    set(CMAKE_REQUIRED_FLAGS -Werror)
  elseif(JEMALLOC_HERROR_ON_WARNING)
    set(CMAKE_REQUIRED_FLAGS -herror_on_warning)
  endif()

  check_c_source_compiles(
    "
    ${header_str}
    int main() {
      ${main_str}
      return 0;
    }"
    ${result_var})
endfunction()

function(jemalloc_check_func func_name result_var)
  check_c_source_compiles(
    "
    extern char ${func_name}();
    int main() {
      return ${func_name}();
    }"
    ${result_var})
endfunction()
