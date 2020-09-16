# The internal defs take up the most space, and are the most tedious.

jemalloc_check_func(__libc_calloc JEMALLOC_OVERRIDE___LIBC_CALLOC)
jemalloc_check_func(__libc_free JEMALLOC_OVERRIDE___LIBC_FREE)
jemalloc_check_func(__libc_malloc JEMALLOC_OVERRIDE___LIBC_MALLOC)
jemalloc_check_func(__libc_memalign JEMALLOC_OVERRIDE___LIBC_MEMALIGN)
jemalloc_check_func(__libc_realloc JEMALLOC_OVERRIDE___LIBC_REALLOC)
jemalloc_check_func(__libc_valloc JEMALLOC_OVERRIDE___LIBC_VALLOC)
jemalloc_check_func(__posix_memalign JEMALLOC_OVERRIDE___POSIX_MEMALIGN)

# CPU insturction for pause/yield
set(HAVE_CPU_SPINWAIT 0)
set(CPU_SPINWAIT "")
if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "i686|x86_64")
  if (${MSVC})
    jemalloc_compilable(
      ""
      "_mm_pause();"
      JEMALLOC_HAVE_PAUSE_MSVC)
    if (${JEMALLOC_HAVE_PAUSE_MSVC})
      set(HAVE_CPU_SPINWAIT 1)
      set(CPU_SPINWAIT "_mm_pause")
    endif()
  else()
    jemalloc_compilable(
      ""
      "__asm__ volatile(\"pause\");"
      JEMALLOC_HAVE_PAUSE)
    if(${JEMALLOC_HAVE_PAUSE})
      set(HAVE_CPU_SPINWAIT 1)
      set(CPU_SPINWAIT "__asm__ volatile(\"pause\");")
    endif()
  endif()
elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64|arm")
  set(HAVE_CPU_SPINWAIT 1)
    jemalloc_compilable(
      ""
      "__asm__ volatile(\"yield\");"
      JEMALLOC_HAVE_YIELD)
    if(${JEMALLOC_HAVE_YIELD})
      set(HAVE_CPU_SPINWAIT 1)
      set(CPU_SPINWAIT "__asm__ volatile(\"yield\");")
    endif()
endif()

# LG_VADDR
# Note that LG_VADDR is set to a cache variable with the value "detect" in
# CMakelists.txt.  Our setting here will overwrite it.
if("${LG_VADDR}" STREQUAL "detect")
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(LG_VADDR 32)
  else()
    # We're on a 64-bit platform of some unknown CPU type.
    if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
      # Make a guess.  This *can* be autodetected, but this isn't implemented
      # yet.
      set(LG_VADDR 48)
    elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
      try_run(
        JEMALLOC_X86_VADDR_RESULT JEMALLOC_X86_VADDR_COMPILE_RESULT
        ${CMAKE_CURRENT_BINARY_DIR}/
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/x86_64_vaddr_check.c
        RUN_OUTPUT_VARIABLE JEMALLOC_X86_VADDR_OUTPUT)
      if("${JEMALLOC_X86_VADDR_RESULT}" EQUAL 0 AND
          "${JEMALLOC_X86_VADDR_COMPILE_RESULT}")
        set(LG_VADDR ${JEMALLOC_X86_VADDR_OUTPUT})
      else()
        message(SEND_ERROR
          "cannot determine number of significant virtual address bits")
      endif()
    else()
      set(LG_VADDR 64)
    endif()
  endif()
endif()

jemalloc_compilable(
  "#include <stdint.h>
   #include <stdatomic.h>"
  "
    uint64_t *p = (uint64_t *)0;
    uint64_t x = 1;
    volatile atomic_uint_least64_t *a = (volatile atomic_uint_least64_t *)p;
    uint64_t r = atomic_fetch_add(a, x) + x;
    return r == 0;
  "
  JEMALLOC_C11_ATOMICS)

jemalloc_compilable(
  ""
  "
    int x = 0;
    int val = 1;
    int y = __atomic_fetch_add(&x, val, __ATOMIC_RELAXED);
    int after_add = x;
    return after_add == 1;
  "
    JEMALLOC_GCC_ATOMIC_ATOMICS)

jemalloc_compilable(
  ""
  "
    unsigned char x = 0;
    int val = 1;
    int y = __atomic_fetch_add(&x, val, __ATOMIC_RELAXED);
    int after_add = (int)x;
    return after_add == 1;
  "
  JEMALLOC_GCC_U8_ATOMIC_ATOMICS)

jemalloc_compilable(
  ""
  "
    int x = 0;
    int before_add = __sync_fetch_and_add(&x, 1);
    int after_add = x;
    return (before_add == 0) && (after_add == 1);
  "
  JEMALLOC_GCC_SYNC_ATOMICS)

jemalloc_compilable(
  ""
  "
    {
      unsigned x = 0;
      int y = __builtin_clz(x);
    }
    {
      unsigned long x = 0;
      int y = __builtin_clzl(x);
    }
  "
  JEMALLOC_HAVE_BUILTIN_CLZ)

jemalloc_compilable(
  "
    #include <os/lock.h>
    #include <AvailabilityMacros.h>
  "
  "
    #if MAC_OS_X_VERSION_MIN_REQUIRED < 101200
    #error \"os_unfair_lock is not supported\"
    #else
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&lock);
    os_unfair_lock_unlock(&lock);
    #endif
  "
  JEMALLOC_OS_UNFAIR_LOCK)

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


