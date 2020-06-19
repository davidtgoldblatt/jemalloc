# Takes an optional additional argument, the malloc conf
function(jemalloc_unit_test name)
  add_executable(${name}
    test/unit/${name}.c
  )
  target_link_libraries(${name}
    PUBLIC
      jemalloc_jet
      testlib_unit
      Threads::Threads
      ${CMAKE_DL_LIBS}
  )

  target_compile_definitions(${name}
    PRIVATE JEMALLOC_UNIT_TEST _GNU_SOURCE
  )

  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(
    ${name}
    PROPERTIES ENVIRONMENT
    MALLOC_CONF=${ARGN})

  # Corresponds to test_status_t.

  set_tests_properties(${name} PROPERTIES
      SKIP_RETURN_CODE 1
  )
endfunction()
