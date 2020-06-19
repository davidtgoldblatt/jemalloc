include(CheckCSourceCompiles)

# Adds flags (extra variadic arguments in argn) to outlist, if they compile
# correctly).
function(jemalloc_compiler_flags_add outlist name)
  set(locallist ${${outlist}})
  set(CMAKE_REQUIRED_FLAGS ${ARGN})
  check_c_source_compiles(
    "
      int main() {
        return 0;
      }
    "
    ${name})
  if(${${name}})
    list(APPEND locallist ${ARGN})
    set(${outlist} ${locallist} PARENT_SCOPE)
  endif()
endfunction()

# Get a list of whatever extra compiler are necessary.
function(jemalloc_compiler_flags_shared outvar)
  set(flags)

  if(CMAKE_C_COMPILER_ID STREQUAL "Cray")
    if (CMAKE_C_COMPILER_VERSION VERSION_EQUAL 8.4)
      # Cray 8.4 has an inlining bug.
      jemalloc_compiler_flags_add(flags JEMALLOC_CRAY84_WORKAROUNDS
        -hipa2 -hnognu)
    endif()
    # Ignore unreachable code warning
    jemalloc_compiler_flags_add(flags JEMALLOC_CRAY_UNREACHABLE -hnomessage=128)
    # Ignore redefinition of "malloc", "free", etc warning
    jemalloc_compiler_flags_add(flags JEMALLOC_CRAY_REDEF -hnomessage=1357)
  endif()

  if (MSVC)
    # Produce debug info, in a separate pdb file.
    jemalloc_compiler_flags_add(flags JEMALLOC_MSVC_ZI /Zi)
    # Use the multithreaded, static version of libraries.
    jemalloc_compiler_flags_add(flags JEMALLOC_MSVC_MT /MT)
    # Highest non-informational warning level.
    jemalloc_compiler_flags_add(flags JEMALLOC_MSVC_W3 /W3)
    # Force synchronization on pdb file access.  In the autoconf world, this was
    # necessary for parallel builds.  I'm not sure if it's still necessary, but
    # added this during a more-or-less translation so I want to minimize
    # changes.
    jemalloc_compiler_flags_add(flags JEMALLOC_MSVC_FS /FS)
  endif()

  check_c_source_compiles("
    #ifndef __GNUC__
    #  error not gnu
    #endif
      int main() {
        return 0;
      }
    "
    gnu_clone)
  if (gnu_clone)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WALL -Wall)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WEXTRA -Wextra)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WSHORTEN_64_TO_32
      -Wshorten-64-to-32)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WSIGN_COMPARE -Wsign-compare)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WUNDEF -Wundef)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WNO_FORMAT_ZERO_LENGTH
      -Wno-format-zero-length)
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WPOINTER_ARITH
      -Wpointer-arith)
    # This warning triggers on the use of the universal zero initializer, which
    # is a very handy idiom for things like the tcache static initializer (which
    # has lots of nested structs).  See the discussion at
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WNO_MISSING_BRACES
      -Wno-missing-braces)
    # This one too.
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_WNO_MISSING_FIELD_INITIALIZERS
      -Wno-missing-field-initializers)
    # Speeds up builds
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_PIPE -pipe)
    # Debug info.
    jemalloc_compiler_flags_add(flags JEMALLOC_GNU_G3 -g3)
  endif()

  set(${outvar} ${flags} PARENT_SCOPE)
endfunction()
