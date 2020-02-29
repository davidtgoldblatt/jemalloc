#ifndef JEMALLOC_INTERNAL_CACHE_BIN_H
#define JEMALLOC_INTERNAL_CACHE_BIN_H

#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sz.h"

/*
 * The cache_bins are the mechanism that the tcache and the arena use to
 * communicate.  The tcache fills from and flushes to the arena by passing a
 * cache_bin_t to fill/flush.  When the arena needs to pull stats from the
 * tcaches associated with it, it does so by iterating over its
 * cache_bin_array_descriptor_t objects and reading out per-bin stats it
 * contains.  This makes it so that the arena need not know about the existence
 * of the tcache at all.
 */

/* The size in bytes of each cache bin stack. */
typedef uint16_t cache_bin_sz_t;

typedef struct cache_bin_stats_s cache_bin_stats_t;
struct cache_bin_stats_s {
	/*
	 * Number of allocation requests that corresponded to the size of this
	 * bin.
	 */
	uint64_t nrequests;
};

/*
 * Read-only information associated with each element of tcache_t's tbins array
 * is stored separately, mainly to reduce memory usage.
 */
typedef struct cache_bin_info_s cache_bin_info_t;
struct cache_bin_info_s {
	/* The size of the bin stack, i.e. ncached_max * sizeof(ptr). */
	cache_bin_sz_t stack_size;
};

typedef struct cache_bin_s cache_bin_t;
struct cache_bin_s {
	/*
	 * The cache bin stack is represented using 3 pointers: cur_ptr,
	 * low_water and full, optimized for the fast path efficiency.
	 *
	 * low addr ==> high addr
	 * |----|----|----|item1|item2|.....................|itemN|
	 *  full            cur                                    empty
	 * (ncached == N; full + ncached_max == empty)
	 *
	 * Data directly stored:
	 * 1) cur_ptr points to the current item to be allocated, i.e. *cur_ptr.
	 * 2) full points to the top of the stack (i.e. ncached == ncached_max),
	 * which is compared against on free_fastpath to check "is_full".
	 * 3) low_water indicates a low water mark of ncached.
	 * Range of low_water is [cur, empty], i.e. values of [ncached, 0].
	 *
	 * The empty position (ncached == 0) is derived via full + ncached_max
	 * and not accessed in the common case (guarded behind low_water).
	 *
	 * On 64-bit, 2 of the 3 pointers (full and low water) are compressed by
	 * omitting the high 32 bits.  Overflow of the half pointers is avoided
	 * when allocating / initializing the stack space.  As a result,
	 * cur_ptr.lowbits can be safely used for pointer comparisons.
	 */
	union {
		void **ptr;
		struct {
			/* highbits never accessed directly. */
#if (LG_SIZEOF_PTR == 3 && defined(JEMALLOC_BIG_ENDIAN))
			uint32_t __highbits;
#endif
			uint32_t lowbits;
#if (LG_SIZEOF_PTR == 3 && !defined(JEMALLOC_BIG_ENDIAN))
			uint32_t __highbits;
#endif
		};
	} cur_ptr;
	/*
	 * cur_ptr and stats are both modified frequently.  Let's keep them
	 * close so that they have a higher chance of being on the same
	 * cacheline, thus less write-backs.
	 */
	cache_bin_stats_t tstats;
	/*
	 * Points to the first item that hasn't been used since last GC, to
	 * track the low water mark (min # of cached).
	 */
	uint32_t low_water_position;
	/*
	 * Points to the position when the cache is full.
	 *
	 * To make use of adjacent cacheline prefetch, the items in the avail
	 * stack goes to higher address for newer allocations (i.e. cur_ptr++).
	 */
	uint32_t full_position;
};

typedef struct cache_bin_array_descriptor_s cache_bin_array_descriptor_t;
struct cache_bin_array_descriptor_s {
	/*
	 * The arena keeps a list of the cache bins associated with it, for
	 * stats collection.
	 */
	ql_elm(cache_bin_array_descriptor_t) link;
	/* Pointers to the tcache bins. */
	cache_bin_t *bins_small;
	cache_bin_t *bins_large;
};

/*
 * None of the cache_bin_*_get / _set functions is used on the fast path, which
 * relies on pointer comparisons to determine if the cache is full / empty.
 */

/* Returns ncached_max: Upper limit on ncached. */
static inline cache_bin_sz_t
cache_bin_info_ncached_max(cache_bin_info_t *info) {
	return info->stack_size / sizeof(void *);
}

static inline cache_bin_sz_t
cache_bin_ncached_get(cache_bin_t *bin, cache_bin_info_t *info) {
	cache_bin_sz_t n = (cache_bin_sz_t)((info->stack_size +
	    bin->full_position - bin->cur_ptr.lowbits) / sizeof(void *));
	assert(n <= cache_bin_info_ncached_max(info));
	assert(n == 0 || *(bin->cur_ptr.ptr) != NULL);

	return n;
}

static inline void **
cache_bin_empty_position_get(cache_bin_t *bin, cache_bin_info_t *info) {
	void **ret = bin->cur_ptr.ptr + cache_bin_ncached_get(bin, info);
	/* Low bits overflow disallowed when allocating the space. */
	assert((uint32_t)(uintptr_t)ret >= bin->cur_ptr.lowbits);

	/* Can also be computed via (full_position + ncached_max) | highbits. */
	uintptr_t lowbits = bin->full_position + info->stack_size;
	uintptr_t highbits = (uintptr_t)bin->cur_ptr.ptr &
	    ~(((uint64_t)1 << 32) - 1);
	assert(ret == (void **)(lowbits | highbits));

	return ret;
}

/* Returns the numeric value of low water in [0, ncached]. */
static inline cache_bin_sz_t
cache_bin_low_water_get(cache_bin_t *bin, cache_bin_info_t *info) {
	cache_bin_sz_t ncached_max = cache_bin_info_ncached_max(info);
	cache_bin_sz_t low_water = ncached_max -
	    (cache_bin_sz_t)((bin->low_water_position - bin->full_position) /
	    sizeof(void *));
	assert(low_water <= ncached_max);
	assert(low_water <= cache_bin_ncached_get(bin, info));
	assert(bin->low_water_position >= bin->cur_ptr.lowbits);

	return low_water;
}

static inline void
cache_bin_ncached_set(cache_bin_t *bin, cache_bin_sz_t n,
    cache_bin_info_t *info) {
	bin->cur_ptr.lowbits = bin->full_position + info->stack_size
	    - n * sizeof(void *);
	assert(n <= cache_bin_info_ncached_max(info));
	assert(n == 0 || *bin->cur_ptr.ptr != NULL);
}

static inline void
cache_bin_array_descriptor_init(cache_bin_array_descriptor_t *descriptor,
    cache_bin_t *bins_small, cache_bin_t *bins_large) {
	ql_elm_new(descriptor, link);
	descriptor->bins_small = bins_small;
	descriptor->bins_large = bins_large;
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy_impl(cache_bin_t *bin, bool *success,
    cache_bin_info_t *info, const bool adjust_low_water) {
	/*
	 * This may read from the empty position; however the loaded value won't
	 * be used.  It's safe because the stack has one more slot reserved.
	 */
	void *ret = *(bin->cur_ptr.ptr++);
	/*
	 * Check for both bin->ncached == 0 and ncached < low_water in a single
	 * branch.  When adjust_low_water is true, this also avoids accessing
	 * the cache_bin_info_ts (which is on a separate cacheline / page) in
	 * the common case.
	 */
	if (unlikely(bin->cur_ptr.lowbits > bin->low_water_position)) {
		if (adjust_low_water) {
			uint32_t empty_position = bin->full_position +
			    info->stack_size;
			if (unlikely(bin->cur_ptr.lowbits > empty_position)) {
				/* Over-allocated; revert. */
				bin->cur_ptr.ptr--;
				assert(bin->cur_ptr.lowbits == empty_position);
				*success = false;
				return NULL;
			}
			bin->low_water_position = bin->cur_ptr.lowbits;
		} else {
			bin->cur_ptr.ptr--;
			assert(bin->cur_ptr.lowbits == bin->low_water_position);
			*success = false;
			return NULL;
		}
	}

	/*
	 * success (instead of ret) should be checked upon the return of this
	 * function.  We avoid checking (ret == NULL) because there is never a
	 * null stored on the avail stack (which is unknown to the compiler),
	 * and eagerly checking ret would cause pipeline stall (waiting for the
	 * cacheline).
	 */
	*success = true;

	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy_reduced(cache_bin_t *bin, bool *success) {
	/* We don't look at info if we're not adjusting low-water. */
	return cache_bin_alloc_easy_impl(bin, success, NULL, false);
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy(cache_bin_t *bin, bool *success, cache_bin_info_t *info) {
	return cache_bin_alloc_easy_impl(bin, success, info, true);
}

JEMALLOC_ALWAYS_INLINE bool
cache_bin_dalloc_easy(cache_bin_t *bin, void *ptr) {
	if (unlikely(bin->cur_ptr.lowbits == bin->full_position)) {
		return false;
	}

	*(--bin->cur_ptr.ptr) = ptr;
	assert(bin->cur_ptr.lowbits >= bin->full_position);

	return true;
}

typedef struct cache_bin_ptr_array_s cache_bin_ptr_array_t;
struct cache_bin_ptr_array_s {
	cache_bin_sz_t n;
	void **ptr;
};

#define CACHE_BIN_PTR_ARRAY_DECLARE(name, nval)				\
    cache_bin_ptr_array_t name;						\
    name.n = (nval)

static inline void
cache_bin_ptr_array_init_for_flush(cache_bin_ptr_array_t *arr, cache_bin_t *bin,
    cache_bin_sz_t nflush, cache_bin_info_t *info) {
	arr->ptr = cache_bin_empty_position_get(bin, info) - 1;
	assert(cache_bin_ncached_get(bin, info) == 0
	    || *arr->ptr != NULL);
}

static inline void
cache_bin_ptr_array_init_for_fill(cache_bin_ptr_array_t *arr, cache_bin_t *bin,
    cache_bin_sz_t nfill, cache_bin_info_t *info) {
	arr->ptr = cache_bin_empty_position_get(bin, info) - nfill;
	assert(cache_bin_ncached_get(bin, info) == 0);
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_ptr_array_get(cache_bin_ptr_array_t *arr, cache_bin_sz_t n) {
	return *(arr->ptr - n);
}

JEMALLOC_ALWAYS_INLINE void
cache_bin_ptr_array_set(cache_bin_ptr_array_t *arr, cache_bin_sz_t n, void *p) {
	*(arr->ptr - n) = p;
}

static inline void
cache_bin_fill_from_ptr_array(cache_bin_t *bin, cache_bin_ptr_array_t *arr,
    szind_t nfilled, cache_bin_info_t *info) {
	assert(cache_bin_ncached_get(bin, info) == 0);
	if (nfilled < arr->n) {
		void **empty_position = cache_bin_empty_position_get(bin, info);
		memmove(empty_position - nfilled, empty_position - arr->n,
		    nfilled * sizeof(void *));
	}
	cache_bin_ncached_set(bin, nfilled, info);
}

#endif /* JEMALLOC_INTERNAL_CACHE_BIN_H */
