# TODO: CFLAGS append order is inverted (currently, cmake-specified flags take
# priority over env var ones; instead, env var should go last.
# TODO: we hardcode in that we find threads, -ldl (via ${CMAKE_DL_LIBS})
# TODO: kill configure_file of the public jemalloc.h
# TODO: figure out smallocx thing

cmake_minimum_required(VERSION 3.2 FATAL_ERROR)

set(CMAKE_MODULE_PATH
  ${CMAKE_MODULE_PATH}
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
)

enable_testing()

option(JEMALLOC_ENABLE_CXX "Enable C++ integration." ON)
set(langs C)
if(JEMALLOC_ENABLE_CXX)
  list(APPEND langs CXX)
endif()

project(jemalloc
  VERSION 5.2.1
  LANGUAGES ${langs}
)

find_package(Threads REQUIRED)

set(JEMALLOC_PREFIX 0)
set(JEMALLOC_CPREFIX 0)

# Stuff in jemalloc/defs.h
set(JEMALLOC_HAVE_ATTR 1)
set(JEMALLOC_HAVE_ATTR_ALLOC_SIZE 1)
set(JEMALLOC_HAVE_ATTR_FORMAT_ARG 1)
set(JEMALLOC_HAVE_ATTR_FORMAT_GNU_PRINTF 1)
set(JEMALLOC_HAVE_ATTR_FORMAT_PRINTF 1)
set(JEMALLOC_HAVE_ATTR_FALLTHROUGH 1)
set(JEMALLOC_OVERRIDE_MEMALIGN 1)
set(JEMALLOC_OVERRIDE_VALLOC 1)
set(JEMALLOC_USABLE_SIZE_CONST 1)
set(JEMALLOC_USE_CXX_THROW 1)
set(LG_SIZEOF_PTR 3)

# Stuff in macros.h
# Obviously, need to do a better job detecting this stuff.
set(jemalloc_version 5.2.1-437-geacdc8f96c5d511c1b2fb6afa9a4f588c5726af3)
set(jemalloc_version_major 5)
set(jemalloc_version_minor 2)
set(jemalloc_version_bugfix 1)
set(jemalloc_version_nrev 437)
set(jemalloc_version_gid eacdc8f96c5d511c1b2fb6afa9a4f588c5726af3)

# Stuff in jemalloc/internal/defs.h
set(JEMALLOC_OVERRIDE___LIBC_CALLOC 1)
set(JEMALLOC_OVERRIDE___LIBC_FREE 1)
set(JEMALLOC_OVERRIDE___LIBC_MALLOC 1)
set(JEMALLOC_OVERRIDE___LIBC_MEMALIGN 1)
set(JEMALLOC_OVERRIDE___LIBC_REALLOC 1)
set(JEMALLOC_OVERRIDE___LIBC_VALLOC 1)
set(JEMALLOC_OVERRIDE___POSIX_MEMALIGN 0)
set(JEMALLOC_PRIVATE_NAMESPACE ${private_namespace})
set(CPU_SPINWAIT "__asm__ volatile(\"pause\")")
set(HAVE_CPU_SPINWAIT 1)
set(LG_VADDR 48)
set(JEMALLOC_C11_ATOMICS 1)
set(JEMALLOC_GCC_ATOMIC_ATOMICS 1)
set(JEMALLOC_GCC_SYNC_ATOMICS 1)
set(JEMALLOC_GCC_U8_ATOMIC_ATOMICS 1)
set(JEMALLOC_HAVE_BUILTIN_CLZ 1)
set(JEMALLOC_OS_UNFAIR_LOCK 0)
set(JEMALLOC_USE_SYSCALL 0)
set(JEMALLOC_HAVE_SECURE_GETENV 1)
set(JEMALLOC_HAVE_ISSETUGID 0)
set(JEMALLOC_HAVE_PTHREAD_ATFORK 1)
set(JEMALLOC_HAVE_PTHREAD_SETNAME_NP 1)
set(JEMALLOC_HAVE_CLOCK_MONOTONIC_COARSE 1)
set(JEMALLOC_HAVE_CLOCK_MONOTONIC 1)
set(JEMALLOC_HAVE_MACH_ABSOLUTE_TIME 0)
set(JEMALLOC_HAVE_CLOCK_REALTIME 1)
set(JEMALLOC_MALLOC_THREAD_CLEANUP 0)
set(JEMALLOC_THREADED_INIT 1)
set(JEMALLOC_TLS_MODEL "__attribute__((tls_model(\"initial-exec\")))")
set(JEMALLOC_DEBUG 0)
set(JEMALLOC_STATS 1)
set(JEMALLOC_EXPERIMENTAL_SMALLOCX_API 0)
set(JEMALLOC_PROF 0)
set(JEMALLOC_PROF_LIBUNWIND 0)
set(JEMALLOC_PROF_LIBGCC 0)
set(JEMALLOC_PROF_GCC 0)
set(JEMALLOC_DSS 1)
set(JEMALLOC_FILL 1)
set(JEMALLOC_UTRACE 0)
set(JEMALLOC_XMALLOC 0)
set(JEMALLOC_LAZY_LOCK 0)
set(LG_QUANTUM 0)
set(LG_PAGE 12)
set(LG_HUGEPAGE 21)
set(JEMALLOC_MAPS_COALESCE 1)
set(JEMALLOC_RETAIN 1)
set(JEMALLOC_TLS 1)
set(JEMALLOC_INTERNAL_UNREACHABLE __builtin_unreachable)
set(JEMALLOC_INTERNAL_FFSLL __builtin_ffsll)
set(JEMALLOC_INTERNAL_FFSL __builtin_ffsl)
set(JEMALLOC_INTERNAL_FFS __builtin_ffs)
set(JEMALLOC_INTERNAL_POPCOUNTL __builtin_popcountl)
set(JEMALLOC_INTERNAL_POPCOUNT __builtin_popcount)
set(JEMALLOC_CACHE_OBLIVIOUS 1)
set(JEMALLOC_LOG 0)
set(JEMALLOC_READLINKAT 0)
set(JEMALLOC_ZONE 0)
set(JEMALLOC_SYSCTL_VM_OVERCOMMIT 0)
set(JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY 1)
set(JEMALLOC_HAVE_MADVISE 1)
set(JEMALLOC_HAVE_MADVISE_HUGE 1)
set(JEMALLOC_PURGE_MADVISE_FREE 1)
set(JEMALLOC_PURGE_MADVISE_DONTNEED 1)
set(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS 1)
set(JEMALLOC_DEFINE_MADVISE_FREE 0)
set(JEMALLOC_MADVISE_DONTDUMP 1)
set(JEMALLOC_THP 0)
set(JEMALLOC_HAS_ALLOCA_H 1)
set(JEMALLOC_HAS_RESTRICT 1)
set(JEMALLOC_BIG_ENDIAN 0)
set(LG_SIZEOF_INT 2)
set(LG_SIZEOF_INT 2)
set(LG_SIZEOF_LONG 3)
set(LG_SIZEOF_LONG_LONG 3)
set(LG_SIZEOF_INTMAX_T 3)
set(JEMALLOC_GLIBC_MALLOC_HOOK 1)
set(JEMALLOC_GLIBC_MEMALIGN_HOOK 0)
set(JEMALLOC_HAVE_PTHREAD 1)
set(JEMALLOC_HAVE_DLSYM 1)
set(JEMALLOC_HAVE_PTHREAD_MUTEX_ADAPTIVE_NP 1)
set(JEMALLOC_HAVE_SCHED_GETCPU 1)
set(JEMALLOC_HAVE_SCHED_SETAFFINITY 1)
set(JEMALLOC_BACKGROUND_THREAD 1)
set(JEMALLOC_EXPORT 0)
set(JEMALLOC_CONFIG_MALLOC_CONF "\"\"")
set(JEAMLLOC_IS_MALLOC 1)
set(JEMALLOC_STRERROR_R_RETURNS_CHAR_WITH_GNU_SOURCE 1)
set(JEMALLOC_OPT_SAFETY_CHECKS 0)

#TODO: need to figure out smallocx handling here.
set(public_syms
  aligned_alloc
  calloc
  dallocx
  free
  mallctl
  mallctlbymib
  mallctlnametomib
  malloc
  malloc_conf
  malloc_conf_2_conf_harder
  malloc_message
  malloc_stats_print
  malloc_usable_size
  mallocx
  nallocx
  posix_memalign
  rallocx
  realloc
  sallocx
  sdallocx
  xallocx
)
if(JEMALLOC_OVERRIDE_MEMALIGN)
  list(APPEND public_syms memalign)
endif()
if(JEMALLOC_OVERRIDE_VALLOC)
  list(APPEND public_syms valloc)
endif()

foreach(sym ${public_syms})
  set(SYM_${sym} ${sym})
endforeach()
foreach(mangled_sym_pair ${mangling})
  string(REPLACE ":" ";" mangled_sym_list ${mangled_sym_pair})
  list(GET mangled_sym_list 0 name)
  list(GET mangled_sym_list 1 mangled_name)
  set(SYM_${name} ${mangled_name})
endforeach()

set(install_suffix "")
set(private_namespace je_)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/internal/defs.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/internal/defs.h
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/internal/preamble.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/internal/preamble.h
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/test/include/test/jemalloc_test.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/test/include/test/jemalloc_test.h.in
)


configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/defs.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/defs.h
)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/rename.h
"/*
 * Name mangling for public symbols is controlled by --with-mangling and
 * --with-jemalloc-prefix.  With default settings the je_ prefix is stripped by
 * these macro definitions.
 */
#ifndef JEMALLOC_NO_RENAME\n"
)
foreach(sym ${public_syms})
  file(APPEND
    ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/rename.h
    "  #define je_${sym} ${SYM_${sym}}\n")
endforeach()

file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/rename.h
  "#endif
")


configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/macros.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/macros.h
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/protos.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/protos.h
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/jemalloc/typedefs.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/typedefs.h
)

# TODO make mangle.h
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/mangle.h
"/*
 * By default application code must explicitly refer to mangled symbol names,
 * so that it is possible to use jemalloc in conjunction with another allocator
 * in the same application.  Define JEMALLOC_MANGLE in order to cause automatic
 * name mangling that matches the API prefixing that happened as a result of
 * --with-mangling and/or --with-jemalloc-prefix configuration settings.
 */
#ifdef JEMALLOC_MANGLE
#  ifndef JEMALLOC_NO_DEMANGLE
#    define JEMALLOC_NO_DEMANGLE
#  endif
")
foreach(sym ${public_syms})
  file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/mangle.h
    "#  define ${sym} je_${sym}\n")
endforeach()

file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/mangle.h
"#endif

/*
 * The je_* macros can be used as stable alternative names for the
 * public jemalloc API if JEMALLOC_NO_DEMANGLE is defined.  This is primarily
 * meant for use in jemalloc itself, but it can be used by application code to
 * provide isolation from the name mangling specified via --with-mangling
 * and/or --with-jemalloc-prefix.
 */
#ifndef JEMALLOC_NO_DEMANGLE
")

foreach(sym ${public_syms})
  file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/mangle.h
    "#  undef je_${sym}\n")
endforeach()
file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/mangle.h "#endif\n")

function(cat IN_FILE OUT_FILE)
    file(READ ${IN_FILE} CONTENTS)
      file(APPEND ${OUT_FILE} "${CONTENTS}")
    endfunction()

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
"#ifndef JEMALLOC_H_
#define JEMALLOC_H_
#ifdef __cplusplus
extern \"C\" {
#endif

")

file(READ
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/defs.h
  defs_contents)
file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  "${defs_contents}")

file(READ
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/rename.h
  rename_contents)
file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  "${rename_contents}")

file(READ
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/macros.h
  macros_contents)
file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  "${macros_contents}")

file(READ
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/protos.h
  protos_contents)
file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  "${protos_contents}")

file(READ
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/typedefs.h
  typedefs_contents)
file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  "${typedefs_contents}")

file(READ
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/mangle.h
  mangle_contents)
file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  "${mangle_contents}")

file(APPEND
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
"#ifdef __cplusplus
}
#endif
#endif /* JEMALLOC_H_ */
")

configure_file(
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/jemalloc.h
)

file(WRITE
  ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/internal/public_namespace.h
  "/* Turns je_foo into foo's post-mangling name. */\n"
)
foreach(sym ${public_syms})
  file(APPEND
    ${CMAKE_CURRENT_BINARY_DIR}/include/jemalloc/internal/public_namespace.h
    "#define je_${sym} JEMALLOC_N(${sym})\n")
endforeach()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/test/include/test/jemalloc_test.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/test/include/test/jemalloc_test.h)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/test/include/test/test_defs.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/test/include/test/test_defs.h)

set(c_srcs
  src/arena.c
  src/background_thread.c
  src/base.c
  src/bin.c
  src/bin_info.c
  src/bitmap.c
  src/buf_writer.c
  src/cache_bin.c
  src/ckh.c
  src/counter.c
  src/ctl.c
  src/decay.c
  src/div.c
  src/ecache.c
  src/edata.c
  src/edata_cache.c
  src/ehooks.c
  src/emap.c
  src/eset.c
  src/extent.c
  src/extent_dss.c
  src/extent_mmap.c
  src/hash.c
  src/hook.c
  src/inspect.c
  src/jemalloc.c
  src/large.c
  src/log.c
  src/malloc_io.c
  src/mutex.c
  src/mutex_pool.c
  src/nstime.c
  src/pa.c
  src/pa_extra.c
  src/pages.c
  src/peak_event.c
  src/prng.c
  src/prof.c
  src/prof_data.c
  src/prof_log.c
  src/prof_recent.c
  src/rtree.c
  src/safety_check.c
  src/sc.c
  src/stats.c
  src/sz.c
  src/tcache.c
  src/test_hooks.c
  src/thread_event.c
  src/ticker.c
  src/tsd.c
  src/witness.c
)

if(APPLE)
  list(
    APPEND c_srcs
    src/zone.c
  )
endif()

set(cpp_srcs
  src/jemalloc_cpp.cpp
)

include(CompilerFlags)
jemalloc_compiler_flags_shared(compiler_flags)

set(jemalloc_lib_srcs
  ${c_srcs}
  ${cpp_srcs}
)
set(jemalloc_lib_includes
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
set(jemalloc_compile_defs
  PRIVATE
    _GNU_SOURCE
)
set(jemalloc_compile_opts
  PRIVATE
    ${compiler_flags}
)

set(jemalloc_link_libs
  PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

add_library(jemalloc ${jemalloc_lib_srcs})
target_include_directories(jemalloc ${jemalloc_lib_includes})
target_compile_definitions(jemalloc ${jemalloc_compile_defs})
target_compile_options(jemalloc ${jemalloc_compile_opts})
target_link_libraries(jemalloc ${jemalloc_link_libs})

add_library(jemalloc_jet ${jemalloc_lib_srcs})
target_include_directories(jemalloc_jet ${jemalloc_lib_includes})
target_compile_definitions(jemalloc_jet ${jemalloc_compile_defs})
target_compile_options(jemalloc_jet ${jemalloc_compile_opts})
target_compile_definitions(jemalloc_jet PRIVATE JEMALLOC_JET)
target_link_libraries(jemalloc_jet ${jemalloc_link_libs})

set(testlib_srcs
  test/src/SFMT.c
  test/src/btalloc.c
  test/src/btalloc_0.c
  test/src/btalloc_1.c
  test/src/math.c
  test/src/mtx.c
  test/src/sleep.c
  test/src/test.c
  test/src/thd.c
  test/src/timer.c
)

add_library(testlib_unit
  ${testlib_srcs}
)
target_include_directories(testlib_unit
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/test/include
    ${CMAKE_CURRENT_BINARY_DIR}/test/include
)
target_link_libraries(testlib_unit
  PUBLIC
    jemalloc_jet
)
target_compile_definitions(testlib_unit
  PRIVATE
    JEMALLOC_UNIT_TEST
    _GNU_SOURCE
)

include(UnitTest)
jemalloc_unit_test(a0)
jemalloc_unit_test(
  arena_decay
  "dirty_decay_ms:1000,muzzy_decay_ms:1000,lg_tcache_max:0"
  )
jemalloc_unit_test(arena_reset)
jemalloc_unit_test(
  arena_reset_prof
  "prof:true,lg_prof_sample:0"
  )
jemalloc_unit_test(atomic)
jemalloc_unit_test(background_thread)
jemalloc_unit_test(background_thread_enable)
jemalloc_unit_test(base)
jemalloc_unit_test(
  binshard
  "narenas:1,bin_shards:1-160:16|129-512:4|256-256:8")
jemalloc_unit_test(bitmap)
jemalloc_unit_test(bit_util)
jemalloc_unit_test(buf_writer)
jemalloc_unit_test(cache_bin)
jemalloc_unit_test(ckh)
jemalloc_unit_test(counter)
jemalloc_unit_test(decay)
jemalloc_unit_test(div)
jemalloc_unit_test(edata_cache)
jemalloc_unit_test(emitter)
jemalloc_unit_test(extent_quantize)
jemalloc_unit_test(fork)
jemalloc_unit_test(hash)
jemalloc_unit_test(hook)
jemalloc_unit_test(huge)
jemalloc_unit_test(inspect)
jemalloc_unit_test(
  junk
  #TODO: Really, this should be conditioned on enable-fill
  "abort:false,zero:false,junk:true")
jemalloc_unit_test(
  junk_alloc
  "abort:false,zero:false,junk:alloc")
jemalloc_unit_test(
  junk_free
  "abort:false,zero:false,junk:free")
jemalloc_unit_test(log)
jemalloc_unit_test(mallctl)
jemalloc_unit_test(
  malloc_conf_2
  "dirty_decay_ms:500")
jemalloc_unit_test(malloc_io)
jemalloc_unit_test(math)
jemalloc_unit_test(mq)
jemalloc_unit_test(mtx)
jemalloc_unit_test(nstime)
jemalloc_unit_test(pa)
jemalloc_unit_test(
  pack
  "dirty_decay_ms:0,muzzy_decay_ms:0")
jemalloc_unit_test(pages)
jemalloc_unit_test(peak)
jemalloc_unit_test(ph)
jemalloc_unit_test(prng)
#TODO: These should be conditioned on prof.
jemalloc_unit_test(
  prof_accum
  "prof:true,prof_accum:true,prof_active:false,lg_prof_sample:0")
jemalloc_unit_test(
  prof_active
  "prof:true,prof_thread_active_init:false,lg_prof_sample:0"
  )
jemalloc_unit_test(
  prof_gdump
  "prof:true,prof_active:false,prof_gdump:true")
jemalloc_unit_test(
  prof_idump
  "tcache:false,prof:true,prof_accum:true,prof_active:false,lg_prof_sample:0,lg_prof_interval:0")
jemalloc_unit_test(
  prof_log
  "prof:true,lg_prof_sample:0")
jemalloc_unit_test(
  prof_recent
  "prof:true,lg_prof_sample:0,prof_recent_alloc_max:3")
jemalloc_unit_test(
  prof_reset
  "prof:true,prof_active:false,lg_prof_sample:0,prof_recent_alloc_max:0")
jemalloc_unit_test(
  prof_tctx
  "prof:true,lg_prof_sample:0")
jemalloc_unit_test(
  prof_thread_name
  "prof:true,prof_active:false")
jemalloc_unit_test(
  prof_use_sys_thread_name
  "prof:true,lg_prof_sample:0,prof_experimental_use_sys_thread_name:true")
jemalloc_unit_test(ql)
jemalloc_unit_test(qr)
jemalloc_unit_test(rb)
jemalloc_unit_test(retained)
jemalloc_unit_test(rtree)
jemalloc_unit_test(
  safety_check
  "prof:true,lg_prof_sample:0")
jemalloc_unit_test(sc)
jemalloc_unit_test(seq)
jemalloc_unit_test(SFMT)
jemalloc_unit_test(size_classes)
jemalloc_unit_test(slab)
jemalloc_unit_test(smoothstep)
jemalloc_unit_test(spin)
jemalloc_unit_test(stats)
jemalloc_unit_test(stats_print)
jemalloc_unit_test(test_hooks)
jemalloc_unit_test(
  thread_event
  "prof:true,lg_prof_sample:0")
jemalloc_unit_test(ticker)
jemalloc_unit_test(tsd)
jemalloc_unit_test(witness)
jemalloc_unit_test(
  zero
  "abort:false,junk:false,zero:true" )
jemalloc_unit_test(
  zero_realloc_abort
  "zero_realloc:abort")
jemalloc_unit_test(
  zero_realloc_free
  "zero_realloc:free")
jemalloc_unit_test(
  zero_realloc_strict
  "zero_realloc:strict")
jemalloc_unit_test(
  zero_reallocs
  "zero_realloc:free")
