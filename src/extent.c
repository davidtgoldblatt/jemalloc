#define JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_pool.h"

/******************************************************************************/
/* Data. */

rtree_t		extents_rtree;
/* Keyed by the address of the extent_t being protected. */
mutex_pool_t	extent_mutex_pool;

size_t opt_lg_extent_max_active_fit = LG_EXTENT_MAX_ACTIVE_FIT_DEFAULT;

static bool extent_commit_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length, bool growing_retained);
#ifdef PAGES_CAN_PURGE_LAZY
static bool extent_purge_lazy_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
#endif
static bool extent_purge_lazy_impl(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, extent_t *extent, size_t offset, size_t length,
    bool growing_retained);
#ifdef PAGES_CAN_PURGE_FORCED
static bool extent_purge_forced_default(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);
#endif
static bool extent_purge_forced_impl(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, extent_t *extent, size_t offset, size_t length,
    bool growing_retained);
static bool extent_split_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t size_a, size_t size_b, bool committed,
    unsigned arena_ind);
static extent_t *extent_split_impl(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, extent_t *extent, size_t size_a, szind_t szind_a,
    bool slab_a, size_t size_b, szind_t szind_b, bool slab_b,
    bool growing_retained);
static bool extent_merge_default(extent_hooks_t *extent_hooks, void *addr_a,
    size_t size_a, void *addr_b, size_t size_b, bool committed,
    unsigned arena_ind);
static bool extent_merge_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *a, extent_t *b, bool growing_retained);

const extent_hooks_t extent_hooks_default = {
	ehooks_default_alloc,
	ehooks_default_dalloc,
	ehooks_default_destroy,
	ehooks_default_commit,
	ehooks_default_decommit
#ifdef PAGES_CAN_PURGE_LAZY
	,
	extent_purge_lazy_default
#else
	,
	NULL
#endif
#ifdef PAGES_CAN_PURGE_FORCED
	,
	extent_purge_forced_default
#else
	,
	NULL
#endif
	,
	extent_split_default,
	extent_merge_default
};

/* Used exclusively for gdump triggering. */
static atomic_zu_t curpages;
static atomic_zu_t highpages;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void extent_deregister(tsdn_t *tsdn, extent_t *extent);
static extent_t *extent_recycle(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    eset_t *eset, void *new_addr, size_t usize, size_t pad, size_t alignment,
    bool slab, szind_t szind, bool *zero, bool *commit, bool growing_retained);
static extent_t *extent_try_coalesce(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, rtree_ctx_t *rtree_ctx, eset_t *eset, extent_t *extent,
    bool *coalesced, bool growing_retained);
static void extent_record(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    eset_t *eset, extent_t *extent, bool growing_retained);

/******************************************************************************/

#define ATTR_NONE /* does nothing */

ph_gen(ATTR_NONE, extent_avail_, extent_tree_t, extent_t, ph_link,
    extent_esnead_comp)

#undef ATTR_NONE

typedef enum {
	lock_result_success,
	lock_result_failure,
	lock_result_no_extent
} lock_result_t;

static lock_result_t
extent_rtree_leaf_elm_try_lock(tsdn_t *tsdn, rtree_leaf_elm_t *elm,
    extent_t **result, bool inactive_only) {
	extent_t *extent1 = rtree_leaf_elm_extent_read(tsdn, &extents_rtree,
	    elm, true);

	/* Slab implies active extents and should be skipped. */
	if (extent1 == NULL || (inactive_only && rtree_leaf_elm_slab_read(tsdn,
	    &extents_rtree, elm, true))) {
		return lock_result_no_extent;
	}

	/*
	 * It's possible that the extent changed out from under us, and with it
	 * the leaf->extent mapping.  We have to recheck while holding the lock.
	 */
	extent_lock(tsdn, extent1);
	extent_t *extent2 = rtree_leaf_elm_extent_read(tsdn,
	    &extents_rtree, elm, true);

	if (extent1 == extent2) {
		*result = extent1;
		return lock_result_success;
	} else {
		extent_unlock(tsdn, extent1);
		return lock_result_failure;
	}
}

/*
 * Returns a pool-locked extent_t * if there's one associated with the given
 * address, and NULL otherwise.
 */
static extent_t *
extent_lock_from_addr(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx, void *addr,
    bool inactive_only) {
	extent_t *ret = NULL;
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, &extents_rtree,
	    rtree_ctx, (uintptr_t)addr, false, false);
	if (elm == NULL) {
		return NULL;
	}
	lock_result_t lock_result;
	do {
		lock_result = extent_rtree_leaf_elm_try_lock(tsdn, elm, &ret,
		    inactive_only);
	} while (lock_result == lock_result_failure);
	return ret;
}

static void
extent_addr_randomize(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t alignment) {
	assert(extent_base_get(extent) == extent_addr_get(extent));

	if (alignment < PAGE) {
		unsigned lg_range = LG_PAGE -
		    lg_floor(CACHELINE_CEILING(alignment));
		size_t r;
		if (!tsdn_null(tsdn)) {
			tsd_t *tsd = tsdn_tsd(tsdn);
			r = (size_t)prng_lg_range_u64(
			    tsd_prng_statep_get(tsd), lg_range);
		} else {
			uint64_t stack_value = (uint64_t)(uintptr_t)&r;
			r = (size_t)prng_lg_range_u64(&stack_value, lg_range);
		}
		uintptr_t random_offset = ((uintptr_t)r) << (LG_PAGE -
		    lg_range);
		extent->e_addr = (void *)((uintptr_t)extent->e_addr +
		    random_offset);
		assert(ALIGNMENT_ADDR2BASE(extent->e_addr, alignment) ==
		    extent->e_addr);
	}
}

extent_t *
extent_alloc(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_lock(tsdn, &arena->extent_avail_mtx);
	extent_t *extent = extent_avail_first(&arena->extent_avail);
	if (extent == NULL) {
		malloc_mutex_unlock(tsdn, &arena->extent_avail_mtx);
		return base_alloc_extent(tsdn, arena->base);
	}
	extent_avail_remove(&arena->extent_avail, extent);
	atomic_fetch_sub_zu(&arena->extent_avail_cnt, 1, ATOMIC_RELAXED);
	malloc_mutex_unlock(tsdn, &arena->extent_avail_mtx);
	return extent;
}

void
extent_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
	malloc_mutex_lock(tsdn, &arena->extent_avail_mtx);
	extent_avail_insert(&arena->extent_avail, extent);
	atomic_fetch_add_zu(&arena->extent_avail_cnt, 1, ATOMIC_RELAXED);
	malloc_mutex_unlock(tsdn, &arena->extent_avail_mtx);
}

ph_gen(, extent_heap_, extent_heap_t, extent_t, ph_link, extent_snad_comp)

static bool
extent_try_delayed_coalesce(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, eset_t *eset, extent_t *extent) {
	extent_state_set(extent, extent_state_active);
	bool coalesced;
	extent = extent_try_coalesce(tsdn, arena, ehooks, rtree_ctx, eset,
	    extent, &coalesced, false);
	extent_state_set(extent, eset_state_get(eset));

	if (!coalesced) {
		return true;
	}
	eset_insert_locked(tsdn, eset, extent);
	return false;
}

extent_t *
extents_alloc(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	assert(size + pad != 0);
	assert(alignment != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	extent_t *extent = extent_recycle(tsdn, arena, ehooks, eset, new_addr,
	    size, pad, alignment, slab, szind, zero, commit, false);
	assert(extent == NULL || extent_dumpable_get(extent));
	return extent;
}

void
extents_dalloc(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    extent_t *extent) {
	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);
	assert(extent_dumpable_get(extent));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	extent_addr_set(extent, extent_base_get(extent));
	extent_zeroed_set(extent, false);

	extent_record(tsdn, arena, ehooks, eset, extent, false);
}

extent_t *
extents_evict(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    size_t npages_min) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	malloc_mutex_lock(tsdn, &eset->mtx);

	/*
	 * Get the LRU coalesced extent, if any.  If coalescing was delayed,
	 * the loop will iterate until the LRU extent is fully coalesced.
	 */
	extent_t *extent;
	while (true) {
		/* Get the LRU extent, if any. */
		extent = extent_list_first(&eset->lru);
		if (extent == NULL) {
			goto label_return;
		}
		/* Check the eviction limit. */
		size_t extents_npages = atomic_load_zu(&eset->npages,
		    ATOMIC_RELAXED);
		if (extents_npages <= npages_min) {
			extent = NULL;
			goto label_return;
		}
		eset_remove_locked(tsdn, eset, extent);
		if (!eset->delay_coalesce) {
			break;
		}
		/* Try to coalesce. */
		if (extent_try_delayed_coalesce(tsdn, arena, ehooks, rtree_ctx,
		    eset, extent)) {
			break;
		}
		/*
		 * The LRU extent was just coalesced and the result placed in
		 * the LRU at its neighbor's position.  Start over.
		 */
	}

	/*
	 * Either mark the extent active or deregister it to protect against
	 * concurrent operations.
	 */
	switch (eset_state_get(eset)) {
	case extent_state_active:
		not_reached();
	case extent_state_dirty:
	case extent_state_muzzy:
		extent_state_set(extent, extent_state_active);
		break;
	case extent_state_retained:
		extent_deregister(tsdn, extent);
		break;
	default:
		not_reached();
	}

label_return:
	malloc_mutex_unlock(tsdn, &eset->mtx);
	return extent;
}

/*
 * This can only happen when we fail to allocate a new extent struct (which
 * indicates OOM), e.g. when trying to split an existing extent.
 */
static void
extents_abandon_vm(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    extent_t *extent, bool growing_retained) {
	size_t sz = extent_size_get(extent);
	if (config_stats) {
		arena_stats_accum_zu(&arena->stats.abandoned_vm, sz);
	}
	/*
	 * Leak extent after making sure its pages have already been purged, so
	 * that this is only a virtual memory leak.
	 */
	if (eset_state_get(eset) == extent_state_dirty) {
		if (extent_purge_lazy_impl(tsdn, arena, ehooks, extent, 0, sz,
		    growing_retained)) {
			extent_purge_forced_impl(tsdn, arena, ehooks, extent, 0,
			    extent_size_get(extent), growing_retained);
		}
	}
	extent_dalloc(tsdn, arena, extent);
}

static void
extent_deactivate_locked(tsdn_t *tsdn, arena_t *arena, eset_t *eset,
    extent_t *extent) {
	assert(extent_arena_ind_get(extent) == arena_ind_get(arena));
	assert(extent_state_get(extent) == extent_state_active);

	extent_state_set(extent, eset_state_get(eset));
	eset_insert_locked(tsdn, eset, extent);
}

static void
extent_deactivate(tsdn_t *tsdn, arena_t *arena, eset_t *eset,
    extent_t *extent) {
	malloc_mutex_lock(tsdn, &eset->mtx);
	extent_deactivate_locked(tsdn, arena, eset, extent);
	malloc_mutex_unlock(tsdn, &eset->mtx);
}

static void
extent_activate_locked(tsdn_t *tsdn, arena_t *arena, eset_t *eset,
    extent_t *extent) {
	assert(extent_arena_ind_get(extent) == arena_ind_get(arena));
	assert(extent_state_get(extent) == eset_state_get(eset));

	eset_remove_locked(tsdn, eset, extent);
	extent_state_set(extent, extent_state_active);
}

static bool
extent_rtree_leaf_elms_lookup(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    const extent_t *extent, bool dependent, bool init_missing,
    rtree_leaf_elm_t **r_elm_a, rtree_leaf_elm_t **r_elm_b) {
	*r_elm_a = rtree_leaf_elm_lookup(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_base_get(extent), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL) {
		return true;
	}
	assert(*r_elm_a != NULL);

	*r_elm_b = rtree_leaf_elm_lookup(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_last_get(extent), dependent, init_missing);
	if (!dependent && *r_elm_b == NULL) {
		return true;
	}
	assert(*r_elm_b != NULL);

	return false;
}

static void
extent_rtree_write_acquired(tsdn_t *tsdn, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b, extent_t *extent, szind_t szind, bool slab) {
	rtree_leaf_elm_write(tsdn, &extents_rtree, elm_a, extent, szind, slab);
	if (elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, elm_b, extent, szind,
		    slab);
	}
}

static void
extent_interior_register(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx, extent_t *extent,
    szind_t szind) {
	assert(extent_slab_get(extent));

	/* Register interior. */
	for (size_t i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE), extent, szind, true);
	}
}

static void
extent_gdump_add(tsdn_t *tsdn, const extent_t *extent) {
	cassert(config_prof);
	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (opt_prof && extent_state_get(extent) == extent_state_active) {
		size_t nadd = extent_size_get(extent) >> LG_PAGE;
		size_t cur = atomic_fetch_add_zu(&curpages, nadd,
		    ATOMIC_RELAXED) + nadd;
		size_t high = atomic_load_zu(&highpages, ATOMIC_RELAXED);
		while (cur > high && !atomic_compare_exchange_weak_zu(
		    &highpages, &high, cur, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
			/*
			 * Don't refresh cur, because it may have decreased
			 * since this thread lost the highpages update race.
			 * Note that high is updated in case of CAS failure.
			 */
		}
		if (cur > high && prof_gdump_get_unlocked()) {
			prof_gdump(tsdn);
		}
	}
}

static void
extent_gdump_sub(tsdn_t *tsdn, const extent_t *extent) {
	cassert(config_prof);

	if (opt_prof && extent_state_get(extent) == extent_state_active) {
		size_t nsub = extent_size_get(extent) >> LG_PAGE;
		assert(atomic_load_zu(&curpages, ATOMIC_RELAXED) >= nsub);
		atomic_fetch_sub_zu(&curpages, nsub, ATOMIC_RELAXED);
	}
}

static bool
extent_register_impl(tsdn_t *tsdn, extent_t *extent, bool gdump_add) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *elm_a, *elm_b;

	/*
	 * We need to hold the lock to protect against a concurrent coalesce
	 * operation that sees us in a partial state.
	 */
	extent_lock(tsdn, extent);

	if (extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, extent, false, true,
	    &elm_a, &elm_b)) {
		extent_unlock(tsdn, extent);
		return true;
	}

	szind_t szind = extent_szind_get_maybe_invalid(extent);
	bool slab = extent_slab_get(extent);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, extent, szind, slab);
	if (slab) {
		extent_interior_register(tsdn, rtree_ctx, extent, szind);
	}

	extent_unlock(tsdn, extent);

	if (config_prof && gdump_add) {
		extent_gdump_add(tsdn, extent);
	}

	return false;
}

static bool
extent_register(tsdn_t *tsdn, extent_t *extent) {
	return extent_register_impl(tsdn, extent, true);
}

static bool
extent_register_no_gdump_add(tsdn_t *tsdn, extent_t *extent) {
	return extent_register_impl(tsdn, extent, false);
}

static void
extent_reregister(tsdn_t *tsdn, extent_t *extent) {
	bool err = extent_register(tsdn, extent);
	assert(!err);
}

/*
 * Removes all pointers to the given extent from the global rtree indices for
 * its interior.  This is relevant for slab extents, for which we need to do
 * metadata lookups at places other than the head of the extent.  We deregister
 * on the interior, then, when an extent moves from being an active slab to an
 * inactive state.
 */
static void
extent_interior_deregister(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    extent_t *extent) {
	size_t i;

	assert(extent_slab_get(extent));

	for (i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}

/*
 * Removes all pointers to the given extent from the global rtree.
 */
static void
extent_deregister_impl(tsdn_t *tsdn, extent_t *extent, bool gdump) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *elm_a, *elm_b;
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, extent, true, false,
	    &elm_a, &elm_b);

	extent_lock(tsdn, extent);

	extent_rtree_write_acquired(tsdn, elm_a, elm_b, NULL, SC_NSIZES, false);
	if (extent_slab_get(extent)) {
		extent_interior_deregister(tsdn, rtree_ctx, extent);
		extent_slab_set(extent, false);
	}

	extent_unlock(tsdn, extent);

	if (config_prof && gdump) {
		extent_gdump_sub(tsdn, extent);
	}
}

static void
extent_deregister(tsdn_t *tsdn, extent_t *extent) {
	extent_deregister_impl(tsdn, extent, true);
}

static void
extent_deregister_no_gdump_sub(tsdn_t *tsdn, extent_t *extent) {
	extent_deregister_impl(tsdn, extent, false);
}

/*
 * Tries to find and remove an extent from eset that can be used for the
 * given allocation request.
 */
static extent_t *
extent_recycle_extract(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, eset_t *eset, void *new_addr, size_t size,
    size_t pad, size_t alignment, bool slab, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(alignment > 0);
	if (config_debug && new_addr != NULL) {
		/*
		 * Non-NULL new_addr has two use cases:
		 *
		 *   1) Recycle a known-extant extent, e.g. during purging.
		 *   2) Perform in-place expanding reallocation.
		 *
		 * Regardless of use case, new_addr must either refer to a
		 * non-existing extent, or to the base of an extant extent,
		 * since only active slabs support interior lookups (which of
		 * course cannot be recycled).
		 */
		assert(PAGE_ADDR2BASE(new_addr) == new_addr);
		assert(pad == 0);
		assert(alignment <= PAGE);
	}

	size_t esize = size + pad;
	malloc_mutex_lock(tsdn, &eset->mtx);
	extent_t *extent;
	if (new_addr != NULL) {
		extent = extent_lock_from_addr(tsdn, rtree_ctx, new_addr,
		    false);
		if (extent != NULL) {
			/*
			 * We might null-out extent to report an error, but we
			 * still need to unlock the associated mutex after.
			 */
			extent_t *unlock_extent = extent;
			assert(extent_base_get(extent) == new_addr);
			if (extent_arena_ind_get(extent)
			    != arena_ind_get(arena) ||
			    extent_size_get(extent) < esize ||
			    extent_state_get(extent) !=
			    eset_state_get(eset)) {
				extent = NULL;
			}
			extent_unlock(tsdn, unlock_extent);
		}
	} else {
		extent = eset_fit_locked(tsdn, eset, esize, alignment);
	}
	if (extent == NULL) {
		malloc_mutex_unlock(tsdn, &eset->mtx);
		return NULL;
	}

	extent_activate_locked(tsdn, arena, eset, extent);
	malloc_mutex_unlock(tsdn, &eset->mtx);

	return extent;
}

/*
 * Given an allocation request and an extent guaranteed to be able to satisfy
 * it, this splits off lead and trail extents, leaving extent pointing to an
 * extent satisfying the allocation.
 * This function doesn't put lead or trail into any eset_t; it's the caller's
 * job to ensure that they can be reused.
 */
typedef enum {
	/*
	 * Split successfully.  lead, extent, and trail, are modified to extents
	 * describing the ranges before, in, and after the given allocation.
	 */
	extent_split_interior_ok,
	/*
	 * The extent can't satisfy the given allocation request.  None of the
	 * input extent_t *s are touched.
	 */
	extent_split_interior_cant_alloc,
	/*
	 * In a potentially invalid state.  Must leak (if *to_leak is non-NULL),
	 * and salvage what's still salvageable (if *to_salvage is non-NULL).
	 * None of lead, extent, or trail are valid.
	 */
	extent_split_interior_error
} extent_split_interior_result_t;

static extent_split_interior_result_t
extent_split_interior(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx,
    /* The result of splitting, in case of success. */
    extent_t **extent, extent_t **lead, extent_t **trail,
    /* The mess to clean up, in case of error. */
    extent_t **to_leak, extent_t **to_salvage,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool growing_retained) {
	size_t esize = size + pad;
	size_t leadsize = ALIGNMENT_CEILING((uintptr_t)extent_base_get(*extent),
	    PAGE_CEILING(alignment)) - (uintptr_t)extent_base_get(*extent);
	assert(new_addr == NULL || leadsize == 0);
	if (extent_size_get(*extent) < leadsize + esize) {
		return extent_split_interior_cant_alloc;
	}
	size_t trailsize = extent_size_get(*extent) - leadsize - esize;

	*lead = NULL;
	*trail = NULL;
	*to_leak = NULL;
	*to_salvage = NULL;

	/* Split the lead. */
	if (leadsize != 0) {
		*lead = *extent;
		*extent = extent_split_impl(tsdn, arena, ehooks, *lead,
		    leadsize, SC_NSIZES, false, esize + trailsize, szind, slab,
		    growing_retained);
		if (*extent == NULL) {
			*to_leak = *lead;
			*lead = NULL;
			return extent_split_interior_error;
		}
	}

	/* Split the trail. */
	if (trailsize != 0) {
		*trail = extent_split_impl(tsdn, arena, ehooks, *extent, esize,
		    szind, slab, trailsize, SC_NSIZES, false, growing_retained);
		if (*trail == NULL) {
			*to_leak = *extent;
			*to_salvage = *lead;
			*lead = NULL;
			*extent = NULL;
			return extent_split_interior_error;
		}
	}

	if (leadsize == 0 && trailsize == 0) {
		/*
		 * Splitting causes szind to be set as a side effect, but no
		 * splitting occurred.
		 */
		extent_szind_set(*extent, szind);
		if (szind != SC_NSIZES) {
			rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx,
			    (uintptr_t)extent_addr_get(*extent), szind, slab);
			if (slab && extent_size_get(*extent) > PAGE) {
				rtree_szind_slab_update(tsdn, &extents_rtree,
				    rtree_ctx,
				    (uintptr_t)extent_past_get(*extent) -
				    (uintptr_t)PAGE, szind, slab);
			}
		}
	}

	return extent_split_interior_ok;
}

/*
 * This fulfills the indicated allocation request out of the given extent (which
 * the caller should have ensured was big enough).  If there's any unused space
 * before or after the resulting allocation, that space is given its own extent
 * and put back into eset.
 */
static extent_t *
extent_recycle_split(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, eset_t *eset, void *new_addr, size_t size,
    size_t pad, size_t alignment, bool slab, szind_t szind, extent_t *extent,
    bool growing_retained) {
	extent_t *lead;
	extent_t *trail;
	extent_t *to_leak;
	extent_t *to_salvage;

	extent_split_interior_result_t result = extent_split_interior(
	    tsdn, arena, ehooks, rtree_ctx, &extent, &lead, &trail, &to_leak,
	    &to_salvage, new_addr, size, pad, alignment, slab, szind,
	    growing_retained);

	if (!maps_coalesce && result != extent_split_interior_ok
	    && !opt_retain) {
		/*
		 * Split isn't supported (implies Windows w/o retain).  Avoid
		 * leaking the eset.
		 */
		assert(to_leak != NULL && lead == NULL && trail == NULL);
		extent_deactivate(tsdn, arena, eset, to_leak);
		return NULL;
	}

	if (result == extent_split_interior_ok) {
		if (lead != NULL) {
			extent_deactivate(tsdn, arena, eset, lead);
		}
		if (trail != NULL) {
			extent_deactivate(tsdn, arena, eset, trail);
		}
		return extent;
	} else {
		/*
		 * We should have picked an extent that was large enough to
		 * fulfill our allocation request.
		 */
		assert(result == extent_split_interior_error);
		if (to_salvage != NULL) {
			extent_deregister(tsdn, to_salvage);
		}
		if (to_leak != NULL) {
			void *leak = extent_base_get(to_leak);
			extent_deregister_no_gdump_sub(tsdn, to_leak);
			extents_abandon_vm(tsdn, arena, ehooks, eset, to_leak,
			    growing_retained);
			assert(extent_lock_from_addr(tsdn, rtree_ctx, leak,
			    false) == NULL);
		}
		return NULL;
	}
	unreachable();
}

static bool
extent_need_manual_zero(arena_t *arena) {
	/*
	 * Need to manually zero the extent on repopulating if either; 1) non
	 * default extent hooks installed (in which case the purge semantics may
	 * change); or 2) transparent huge pages enabled.
	 */
	return (!ehooks_are_default(arena_get_ehooks(arena)) ||
		(opt_thp == thp_mode_always));
}

/*
 * Tries to satisfy the given allocation request by reusing one of the extents
 * in the given eset_t.
 */
static extent_t *
extent_recycle(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(new_addr == NULL || !slab);
	assert(pad == 0 || !slab);
	assert(!*zero || !slab);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	extent_t *extent = extent_recycle_extract(tsdn, arena, ehooks,
	    rtree_ctx, eset, new_addr, size, pad, alignment, slab,
	    growing_retained);
	if (extent == NULL) {
		return NULL;
	}

	extent = extent_recycle_split(tsdn, arena, ehooks, rtree_ctx, eset,
	    new_addr, size, pad, alignment, slab, szind, extent,
	    growing_retained);
	if (extent == NULL) {
		return NULL;
	}

	if (*commit && !extent_committed_get(extent)) {
		if (extent_commit_impl(tsdn, arena, ehooks, extent, 0,
		    extent_size_get(extent), growing_retained)) {
			extent_record(tsdn, arena, ehooks, eset, extent,
			    growing_retained);
			return NULL;
		}
		if (!extent_need_manual_zero(arena)) {
			extent_zeroed_set(extent, true);
		}
	}

	if (extent_committed_get(extent)) {
		*commit = true;
	}
	if (extent_zeroed_get(extent)) {
		*zero = true;
	}

	if (pad != 0) {
		extent_addr_randomize(tsdn, arena, extent, alignment);
	}
	assert(extent_state_get(extent) == extent_state_active);
	if (slab) {
		extent_slab_set(extent, slab);
		extent_interior_register(tsdn, rtree_ctx, extent, szind);
	}

	if (*zero) {
		void *addr = extent_base_get(extent);
		if (!extent_zeroed_get(extent)) {
			size_t size = extent_size_get(extent);
			if (extent_need_manual_zero(arena) ||
			    pages_purge_forced(addr, size)) {
				memset(addr, 0, size);
			}
		} else if (config_debug) {
			size_t *p = (size_t *)(uintptr_t)addr;
			/* Check the first page only. */
			for (size_t i = 0; i < PAGE / sizeof(size_t); i++) {
				assert(p[i] == 0);
			}
		}
	}
	return extent;
}

static void
extent_hook_pre_reentrancy(tsdn_t *tsdn, arena_t *arena) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	if (arena == arena_get(tsd_tsdn(tsd), 0, false)) {
		/*
		 * The only legitimate case of customized extent hooks for a0 is
		 * hooks with no allocation activities.  One such example is to
		 * place metadata on pre-allocated resources such as huge pages.
		 * In that case, rely on reentrancy_level checks to catch
		 * infinite recursions.
		 */
		pre_reentrancy(tsd, NULL);
	} else {
		pre_reentrancy(tsd, arena);
	}
}

static void
extent_hook_post_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	post_reentrancy(tsd);
}

/*
 * If virtual memory is retained, create increasingly larger extents from which
 * to split requested extents in order to limit the total number of disjoint
 * virtual memory ranges retained by each arena.
 */
static extent_t *
extent_grow_retained(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    size_t size, size_t pad, size_t alignment, bool slab, szind_t szind,
    bool *zero, bool *commit) {
	malloc_mutex_assert_owner(tsdn, &arena->extent_grow_mtx);
	assert(pad == 0 || !slab);
	assert(!*zero || !slab);

	size_t esize = size + pad;
	size_t alloc_size_min = esize + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size_min < esize) {
		goto label_err;
	}
	/*
	 * Find the next extent size in the series that would be large enough to
	 * satisfy this request.
	 */
	pszind_t egn_skip = 0;
	size_t alloc_size = sz_pind2sz(arena->extent_grow_next + egn_skip);
	while (alloc_size < alloc_size_min) {
		egn_skip++;
		if (arena->extent_grow_next + egn_skip >=
		    sz_psz2ind(SC_LARGE_MAXCLASS)) {
			/* Outside legal range. */
			goto label_err;
		}
		alloc_size = sz_pind2sz(arena->extent_grow_next + egn_skip);
	}

	extent_t *extent = extent_alloc(tsdn, arena);
	if (extent == NULL) {
		goto label_err;
	}
	bool zeroed = false;
	bool committed = false;

	void *ptr = ehooks_alloc(tsdn, ehooks, NULL, alloc_size, PAGE, &zeroed,
	    &committed, arena_ind_get(arena));

	extent_init(extent, arena_ind_get(arena), ptr, alloc_size, false,
	    SC_NSIZES, arena_extent_sn_next(arena), extent_state_active, zeroed,
	    committed, true, EXTENT_IS_HEAD);
	if (ptr == NULL) {
		extent_dalloc(tsdn, arena, extent);
		goto label_err;
	}

	if (extent_register_no_gdump_add(tsdn, extent)) {
		extent_dalloc(tsdn, arena, extent);
		goto label_err;
	}

	if (extent_zeroed_get(extent) && extent_committed_get(extent)) {
		*zero = true;
	}
	if (extent_committed_get(extent)) {
		*commit = true;
	}

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	extent_t *lead;
	extent_t *trail;
	extent_t *to_leak;
	extent_t *to_salvage;
	extent_split_interior_result_t result = extent_split_interior(tsdn,
	    arena, ehooks, rtree_ctx, &extent, &lead, &trail, &to_leak,
	    &to_salvage, NULL, size, pad, alignment, slab, szind, true);

	if (result == extent_split_interior_ok) {
		if (lead != NULL) {
			extent_record(tsdn, arena, ehooks,
			    &arena->eset_retained, lead, true);
		}
		if (trail != NULL) {
			extent_record(tsdn, arena, ehooks,
			    &arena->eset_retained, trail, true);
		}
	} else {
		/*
		 * We should have allocated a sufficiently large extent; the
		 * cant_alloc case should not occur.
		 */
		assert(result == extent_split_interior_error);
		if (to_salvage != NULL) {
			if (config_prof) {
				extent_gdump_add(tsdn, to_salvage);
			}
			extent_record(tsdn, arena, ehooks,
			    &arena->eset_retained, to_salvage, true);
		}
		if (to_leak != NULL) {
			extent_deregister_no_gdump_sub(tsdn, to_leak);
			extents_abandon_vm(tsdn, arena, ehooks,
			    &arena->eset_retained, to_leak, true);
		}
		goto label_err;
	}

	if (*commit && !extent_committed_get(extent)) {
		if (extent_commit_impl(tsdn, arena, ehooks, extent, 0,
		    extent_size_get(extent), true)) {
			extent_record(tsdn, arena, ehooks,
			    &arena->eset_retained, extent, true);
			goto label_err;
		}
		if (!extent_need_manual_zero(arena)) {
			extent_zeroed_set(extent, true);
		}
	}

	/*
	 * Increment extent_grow_next if doing so wouldn't exceed the allowed
	 * range.
	 */
	if (arena->extent_grow_next + egn_skip + 1 <=
	    arena->retain_grow_limit) {
		arena->extent_grow_next += egn_skip + 1;
	} else {
		arena->extent_grow_next = arena->retain_grow_limit;
	}
	/* All opportunities for failure are past. */
	malloc_mutex_unlock(tsdn, &arena->extent_grow_mtx);

	if (config_prof) {
		/* Adjust gdump stats now that extent is final size. */
		extent_gdump_add(tsdn, extent);
	}
	if (pad != 0) {
		extent_addr_randomize(tsdn, arena, extent, alignment);
	}
	if (slab) {
		rtree_ctx_t rtree_ctx_fallback;
		rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn,
		    &rtree_ctx_fallback);

		extent_slab_set(extent, true);
		extent_interior_register(tsdn, rtree_ctx, extent, szind);
	}
	if (*zero && !extent_zeroed_get(extent)) {
		void *addr = extent_base_get(extent);
		size_t size = extent_size_get(extent);
		if (extent_need_manual_zero(arena) ||
		    pages_purge_forced(addr, size)) {
			memset(addr, 0, size);
		}
	}

	return extent;
label_err:
	malloc_mutex_unlock(tsdn, &arena->extent_grow_mtx);
	return NULL;
}

static extent_t *
extent_alloc_retained(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	assert(size != 0);
	assert(alignment != 0);

	malloc_mutex_lock(tsdn, &arena->extent_grow_mtx);

	extent_t *extent = extent_recycle(tsdn, arena, ehooks,
	    &arena->eset_retained, new_addr, size, pad, alignment, slab,
	    szind, zero, commit, true);
	if (extent != NULL) {
		malloc_mutex_unlock(tsdn, &arena->extent_grow_mtx);
		if (config_prof) {
			extent_gdump_add(tsdn, extent);
		}
	} else if (opt_retain && new_addr == NULL) {
		extent = extent_grow_retained(tsdn, arena, ehooks, size, pad,
		    alignment, slab, szind, zero, commit);
		/* extent_grow_retained() always releases extent_grow_mtx. */
	} else {
		malloc_mutex_unlock(tsdn, &arena->extent_grow_mtx);
	}
	malloc_mutex_assert_not_owner(tsdn, &arena->extent_grow_mtx);

	return extent;
}

static extent_t *
extent_alloc_wrapper_hard(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	size_t esize = size + pad;
	extent_t *extent = extent_alloc(tsdn, arena);
	if (extent == NULL) {
		return NULL;
	}
	size_t palignment = ALIGNMENT_CEILING(alignment, PAGE);
	void *addr = ehooks_alloc(tsdn, ehooks, new_addr, esize, palignment,
	    zero, commit, arena_ind_get(arena));
	if (addr == NULL) {
		extent_dalloc(tsdn, arena, extent);
		return NULL;
	}
	extent_init(extent, arena_ind_get(arena), addr, esize, slab, szind,
	    arena_extent_sn_next(arena), extent_state_active, *zero, *commit,
	    true, EXTENT_NOT_HEAD);
	if (pad != 0) {
		extent_addr_randomize(tsdn, arena, extent, alignment);
	}
	if (extent_register(tsdn, extent)) {
		extent_dalloc(tsdn, arena, extent);
		return NULL;
	}

	return extent;
}

extent_t *
extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	extent_t *extent = extent_alloc_retained(tsdn, arena, ehooks, new_addr,
	    size, pad, alignment, slab, szind, zero, commit);
	if (extent == NULL) {
		if (opt_retain && new_addr != NULL) {
			/*
			 * When retain is enabled and new_addr is set, we do not
			 * attempt extent_alloc_wrapper_hard which does mmap
			 * that is very unlikely to succeed (unless it happens
			 * to be at the end).
			 */
			return NULL;
		}
		extent = extent_alloc_wrapper_hard(tsdn, arena, ehooks,
		    new_addr, size, pad, alignment, slab, szind, zero, commit);
	}

	assert(extent == NULL || extent_dumpable_get(extent));
	return extent;
}

static bool
extent_can_coalesce(arena_t *arena, eset_t *eset, const extent_t *inner,
    const extent_t *outer) {
	assert(extent_arena_ind_get(inner) == arena_ind_get(arena));
	if (extent_arena_ind_get(outer) != arena_ind_get(arena)) {
		return false;
	}

	assert(extent_state_get(inner) == extent_state_active);
	if (extent_state_get(outer) != eset->state) {
		return false;
	}

	if (extent_committed_get(inner) != extent_committed_get(outer)) {
		return false;
	}

	return true;
}

static bool
extent_coalesce(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    extent_t *inner, extent_t *outer, bool forward, bool growing_retained) {
	assert(extent_can_coalesce(arena, eset, inner, outer));

	extent_activate_locked(tsdn, arena, eset, outer);

	malloc_mutex_unlock(tsdn, &eset->mtx);
	bool err = extent_merge_impl(tsdn, arena, ehooks,
	    forward ? inner : outer, forward ? outer : inner, growing_retained);
	malloc_mutex_lock(tsdn, &eset->mtx);

	if (err) {
		extent_deactivate_locked(tsdn, arena, eset, outer);
	}

	return err;
}

static extent_t *
extent_try_coalesce_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, eset_t *eset, extent_t *extent, bool *coalesced,
    bool growing_retained, bool inactive_only) {
	/*
	 * We avoid checking / locking inactive neighbors for large size
	 * classes, since they are eagerly coalesced on deallocation which can
	 * cause lock contention.
	 */
	/*
	 * Continue attempting to coalesce until failure, to protect against
	 * races with other threads that are thwarted by this one.
	 */
	bool again;
	do {
		again = false;

		/* Try to coalesce forward. */
		extent_t *next = extent_lock_from_addr(tsdn, rtree_ctx,
		    extent_past_get(extent), inactive_only);
		if (next != NULL) {
			/*
			 * eset->mtx only protects against races for
			 * like-state eset, so call extent_can_coalesce()
			 * before releasing next's pool lock.
			 */
			bool can_coalesce = extent_can_coalesce(arena, eset,
			    extent, next);

			extent_unlock(tsdn, next);

			if (can_coalesce && !extent_coalesce(tsdn, arena,
			    ehooks, eset, extent, next, true,
			    growing_retained)) {
				if (eset->delay_coalesce) {
					/* Do minimal coalescing. */
					*coalesced = true;
					return extent;
				}
				again = true;
			}
		}

		/* Try to coalesce backward. */
		extent_t *prev = extent_lock_from_addr(tsdn, rtree_ctx,
		    extent_before_get(extent), inactive_only);
		if (prev != NULL) {
			bool can_coalesce = extent_can_coalesce(arena, eset,
			    extent, prev);
			extent_unlock(tsdn, prev);

			if (can_coalesce && !extent_coalesce(tsdn, arena,
			    ehooks, eset, extent, prev, false,
			    growing_retained)) {
				extent = prev;
				if (eset->delay_coalesce) {
					/* Do minimal coalescing. */
					*coalesced = true;
					return extent;
				}
				again = true;
			}
		}
	} while (again);

	if (eset->delay_coalesce) {
		*coalesced = false;
	}
	return extent;
}

static extent_t *
extent_try_coalesce(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, eset_t *eset, extent_t *extent, bool *coalesced,
    bool growing_retained) {
	return extent_try_coalesce_impl(tsdn, arena, ehooks, rtree_ctx, eset,
	    extent, coalesced, growing_retained, false);
}

static extent_t *
extent_try_coalesce_large(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, eset_t *eset, extent_t *extent, bool *coalesced,
    bool growing_retained) {
	return extent_try_coalesce_impl(tsdn, arena, ehooks, rtree_ctx, eset,
	    extent, coalesced, growing_retained, true);
}

/*
 * Does the metadata management portions of putting an unused extent into the
 * given eset_t (coalesces, deregisters slab interiors, the heap operations).
 */
static void
extent_record(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, eset_t *eset,
    extent_t *extent, bool growing_retained) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	assert((eset_state_get(eset) != extent_state_dirty &&
	    eset_state_get(eset) != extent_state_muzzy) ||
	    !extent_zeroed_get(extent));

	malloc_mutex_lock(tsdn, &eset->mtx);

	extent_szind_set(extent, SC_NSIZES);
	if (extent_slab_get(extent)) {
		extent_interior_deregister(tsdn, rtree_ctx, extent);
		extent_slab_set(extent, false);
	}

	assert(rtree_extent_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_base_get(extent), true) == extent);

	if (!eset->delay_coalesce) {
		extent = extent_try_coalesce(tsdn, arena, ehooks, rtree_ctx,
		    eset, extent, NULL, growing_retained);
	} else if (extent_size_get(extent) >= SC_LARGE_MINCLASS) {
		assert(eset == &arena->eset_dirty);
		/* Always coalesce large eset eagerly. */
		bool coalesced;
		do {
			assert(extent_state_get(extent) == extent_state_active);
			extent = extent_try_coalesce_large(tsdn, arena, ehooks,
			    rtree_ctx, eset, extent, &coalesced,
			    growing_retained);
		} while (coalesced);
		if (extent_size_get(extent) >= oversize_threshold) {
			/* Shortcut to purge the oversize extent eagerly. */
			malloc_mutex_unlock(tsdn, &eset->mtx);
			arena_decay_extent(tsdn, arena, ehooks, extent);
			return;
		}
	}
	extent_deactivate_locked(tsdn, arena, eset, extent);

	malloc_mutex_unlock(tsdn, &eset->mtx);
}

void
extent_dalloc_gap(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
	ehooks_t *ehooks = arena_get_ehooks(arena);

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (extent_register(tsdn, extent)) {
		extent_dalloc(tsdn, arena, extent);
		return;
	}
	extent_dalloc_wrapper(tsdn, arena, ehooks, extent);
}

static bool
extent_may_dalloc(void) {
	/* With retain enabled, the default dalloc always fails. */
	return !opt_retain;
}

static bool
extent_dalloc_wrapper_try(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent) {
	bool err;

	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	extent_addr_set(extent, extent_base_get(extent));

	/* Try to deallocate. */
	err = ehooks_dalloc(tsdn, ehooks, extent_base_get(extent),
	    extent_size_get(extent), extent_committed_get(extent),
	    arena_ind_get(arena));

	if (!err) {
		extent_dalloc(tsdn, arena, extent);
	}

	return err;
}

void
extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent) {
	assert(extent_dumpable_get(extent));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/* Avoid calling the default extent_dalloc unless have to. */
	if (!ehooks_are_default(ehooks) || extent_may_dalloc()) {
		/*
		 * Deregister first to avoid a race with other allocating
		 * threads, and reregister if deallocation fails.
		 */
		extent_deregister(tsdn, extent);
		if (!extent_dalloc_wrapper_try(tsdn, arena, ehooks, extent)) {
			return;
		}
		extent_reregister(tsdn, extent);
	}

	if (!ehooks_are_default(ehooks)) {
		extent_hook_pre_reentrancy(tsdn, arena);
	}
	/* Try to decommit; purge if that fails. */
	bool zeroed;
	if (!extent_committed_get(extent)) {
		zeroed = true;
	} else if (!extent_decommit_wrapper(tsdn, arena, ehooks, extent, 0,
	    extent_size_get(extent))) {
		zeroed = true;
	} else if (!ehooks_purge_forced(ehooks, extent_base_get(extent),
	    extent_size_get(extent), 0, extent_size_get(extent),
	    arena_ind_get(arena))) {
		zeroed = true;
	} else if (extent_state_get(extent) == extent_state_muzzy ||
	    !ehooks_purge_lazy(ehooks, extent_base_get(extent),
	    extent_size_get(extent), 0, extent_size_get(extent),
	    arena_ind_get(arena))) {
		zeroed = false;
	} else {
		zeroed = false;
	}
	if (!ehooks_are_default(ehooks)) {
		extent_hook_post_reentrancy(tsdn);
	}
	extent_zeroed_set(extent, zeroed);

	if (config_prof) {
		extent_gdump_sub(tsdn, extent);
	}

	extent_record(tsdn, arena, ehooks, &arena->eset_retained, extent,
	    false);
}

void
extent_destroy_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent) {
	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/* Deregister first to avoid a race with other allocating threads. */
	extent_deregister(tsdn, extent);

	extent_addr_set(extent, extent_base_get(extent));

	/* Try to destroy; silently fail otherwise. */
	ehooks_destroy(tsdn, ehooks, extent_base_get(extent),
	    extent_size_get(extent), extent_committed_get(extent),
	    arena_ind_get(arena));

	extent_dalloc(tsdn, arena, extent);
}

static bool
extent_commit_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_commit(tsdn, ehooks, extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena_ind_get(arena));
	extent_committed_set(extent, extent_committed_get(extent) || !err);
	return err;
}

bool
extent_commit_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset,
    size_t length) {
	return extent_commit_impl(tsdn, arena, ehooks, extent, offset, length,
	    false);
}

bool
extent_decommit_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	bool err = ehooks_decommit(tsdn, ehooks, extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena_ind_get(arena));
	extent_committed_set(extent, extent_committed_get(extent) && err);
	return err;
}

#ifdef PAGES_CAN_PURGE_LAZY
static bool
extent_purge_lazy_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return pages_purge_lazy((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}
#endif

static bool
extent_purge_lazy_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);

	if (ehooks_purge_lazy_will_fail(ehooks)) {
		return true;
	}
	if (!ehooks_are_default(ehooks)) {
		extent_hook_pre_reentrancy(tsdn, arena);
	}
	bool err = ehooks_purge_lazy(ehooks, extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena_ind_get(arena));
	if (!ehooks_are_default(ehooks)) {
		extent_hook_post_reentrancy(tsdn);
	}

	return err;
}

bool
extent_purge_lazy_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length) {
	return extent_purge_lazy_impl(tsdn, arena, ehooks, extent, offset,
	    length, false);
}

#ifdef PAGES_CAN_PURGE_FORCED
static bool
extent_purge_forced_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind) {
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return pages_purge_forced((void *)((uintptr_t)addr +
	    (uintptr_t)offset), length);
}
#endif

static bool
extent_purge_forced_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);

	if (ehooks_purge_forced_will_fail(ehooks)) {
		return true;
	}
	if (!ehooks_are_default(ehooks)) {
		extent_hook_pre_reentrancy(tsdn, arena);
	}
	bool err = ehooks_purge_forced(ehooks, extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena_ind_get(arena));
	if (!ehooks_are_default(ehooks)) {
		extent_hook_post_reentrancy(tsdn);
	}
	return err;
}

bool
extent_purge_forced_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t offset, size_t length) {
	return extent_purge_forced_impl(tsdn, arena, ehooks, extent,
	    offset, length, false);
}

static bool
extent_split_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	if (!maps_coalesce) {
		/*
		 * Without retain, only whole regions can be purged (required by
		 * MEM_RELEASE on Windows) -- therefore disallow splitting.  See
		 * comments in extent_head_no_merge().
		 */
		return !opt_retain;
	}

	return false;
}

/*
 * Accepts the extent to split, and the characteristics of each side of the
 * split.  The 'a' parameters go with the 'lead' of the resulting pair of
 * extents (the lower addressed portion of the split), and the 'b' parameters go
 * with the trail (the higher addressed portion).  This makes 'extent' the lead,
 * and returns the trail (except in case of error).
 */
static extent_t *
extent_split_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t size_a, szind_t szind_a, bool slab_a,
    size_t size_b, szind_t szind_b, bool slab_b, bool growing_retained) {
	assert(extent_size_get(extent) == size_a + size_b);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);

	if (ehooks_split_will_fail(ehooks)) {
		return NULL;
	}

	extent_t *trail = extent_alloc(tsdn, arena);
	if (trail == NULL) {
		goto label_error_a;
	}

	extent_init(trail, arena_ind_get(arena),
	    (void *)((uintptr_t)extent_base_get(extent) + size_a), size_b,
	    slab_b, szind_b, extent_sn_get(extent), extent_state_get(extent),
	    extent_zeroed_get(extent), extent_committed_get(extent),
	    extent_dumpable_get(extent), EXTENT_NOT_HEAD);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *lead_elm_a, *lead_elm_b;
	{
		extent_t lead;

		extent_init(&lead, arena_ind_get(arena),
		    extent_addr_get(extent), size_a,
		    slab_a, szind_a, extent_sn_get(extent),
		    extent_state_get(extent), extent_zeroed_get(extent),
		    extent_committed_get(extent), extent_dumpable_get(extent),
		    EXTENT_NOT_HEAD);

		extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, &lead, false,
		    true, &lead_elm_a, &lead_elm_b);
	}
	rtree_leaf_elm_t *trail_elm_a, *trail_elm_b;
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, trail, false, true,
	    &trail_elm_a, &trail_elm_b);

	if (lead_elm_a == NULL || lead_elm_b == NULL || trail_elm_a == NULL
	    || trail_elm_b == NULL) {
		goto label_error_b;
	}

	extent_lock2(tsdn, extent, trail);

	if (!ehooks_are_default(ehooks)) {
		extent_hook_pre_reentrancy(tsdn, arena);
	}
	bool err = ehooks_split(ehooks, extent_base_get(extent),
	    size_a + size_b, size_a, size_b, extent_committed_get(extent),
	    arena_ind_get(arena));
	if (!ehooks_are_default(ehooks)) {
		extent_hook_post_reentrancy(tsdn);
	}
	if (err) {
		goto label_error_c;
	}

	extent_size_set(extent, size_a);
	extent_szind_set(extent, szind_a);

	extent_rtree_write_acquired(tsdn, lead_elm_a, lead_elm_b, extent,
	    szind_a, slab_a);
	extent_rtree_write_acquired(tsdn, trail_elm_a, trail_elm_b, trail,
	    szind_b, slab_b);

	extent_unlock2(tsdn, extent, trail);

	return trail;
label_error_c:
	extent_unlock2(tsdn, extent, trail);
label_error_b:
	extent_dalloc(tsdn, arena, trail);
label_error_a:
	return NULL;
}

extent_t *
extent_split_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *extent, size_t size_a, szind_t szind_a, bool slab_a,
    size_t size_b, szind_t szind_b, bool slab_b) {
	return extent_split_impl(tsdn, arena, ehooks, extent, size_a, szind_a,
	    slab_a, size_b, szind_b, slab_b, false);
}

static bool
extent_merge_default_impl(void *addr_a, void *addr_b) {
	if (!maps_coalesce && !opt_retain) {
		return true;
	}
	if (have_dss && !extent_dss_mergeable(addr_a, addr_b)) {
		return true;
	}

	return false;
}

/*
 * Returns true if the given extents can't be merged because of their head bit
 * settings.  Assumes the second extent has the higher address.
 */
static bool
extent_head_no_merge(extent_t *a, extent_t *b) {
	assert(extent_base_get(a) < extent_base_get(b));
	/*
	 * When coalesce is not always allowed (Windows), only merge extents
	 * from the same VirtualAlloc region under opt.retain (in which case
	 * MEM_DECOMMIT is utilized for purging).
	 */
	if (maps_coalesce) {
		return false;
	}
	if (!opt_retain) {
		return true;
	}
	/* If b is a head extent, disallow the cross-region merge. */
	if (extent_is_head_get(b)) {
		/*
		 * Additionally, sn should not overflow with retain; sanity
		 * check that different regions have unique sn.
		 */
		assert(extent_sn_comp(a, b) != 0);
		return true;
	}
	assert(extent_sn_comp(a, b) == 0);

	return false;
}

static bool
extent_merge_default(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	if (!maps_coalesce) {
		tsdn_t *tsdn = tsdn_fetch();
		extent_t *a = iealloc(tsdn, addr_a);
		extent_t *b = iealloc(tsdn, addr_b);
		if (extent_head_no_merge(a, b)) {
			return true;
		}
	}
	return extent_merge_default_impl(addr_a, addr_b);
}

static bool
extent_merge_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, extent_t *a,
    extent_t *b, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(extent_base_get(a) < extent_base_get(b));

	if (ehooks_merge_will_fail(ehooks) || extent_head_no_merge(a, b)) {
		return true;
	}

	bool err;
	if (ehooks_are_default(ehooks)) {
		/* Call directly to propagate tsdn. */
		err = extent_merge_default_impl(extent_base_get(a),
		    extent_base_get(b));
	} else {
		extent_hook_pre_reentrancy(tsdn, arena);
		err = ehooks_merge(ehooks, extent_base_get(a),
		    extent_size_get(a), extent_base_get(b), extent_size_get(b),
		    extent_committed_get(a), arena_ind_get(arena));
		extent_hook_post_reentrancy(tsdn);
	}

	if (err) {
		return true;
	}

	/*
	 * The rtree writes must happen while all the relevant elements are
	 * owned, so the following code uses decomposed helper functions rather
	 * than extent_{,de}register() to do things in the right order.
	 */
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *a_elm_a, *a_elm_b, *b_elm_a, *b_elm_b;
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, a, true, false, &a_elm_a,
	    &a_elm_b);
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, b, true, false, &b_elm_a,
	    &b_elm_b);

	extent_lock2(tsdn, a, b);

	if (a_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, a_elm_b, NULL,
		    SC_NSIZES, false);
	}
	if (b_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, b_elm_a, NULL,
		    SC_NSIZES, false);
	} else {
		b_elm_b = b_elm_a;
	}

	extent_size_set(a, extent_size_get(a) + extent_size_get(b));
	extent_szind_set(a, SC_NSIZES);
	extent_sn_set(a, (extent_sn_get(a) < extent_sn_get(b)) ?
	    extent_sn_get(a) : extent_sn_get(b));
	extent_zeroed_set(a, extent_zeroed_get(a) && extent_zeroed_get(b));

	extent_rtree_write_acquired(tsdn, a_elm_a, b_elm_b, a, SC_NSIZES,
	    false);

	extent_unlock2(tsdn, a, b);

	/*
	 * If we got here, we merged the extents; so they must be from the same
	 * arena (i.e. this one).
	 */
	assert(extent_arena_ind_get(b) == arena_ind_get(arena));
	extent_dalloc(tsdn, arena, b);

	return false;
}

bool
extent_merge_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    extent_t *a, extent_t *b) {
	return extent_merge_impl(tsdn, arena, ehooks, a, b, false);
}

bool
extent_boot(void) {
	if (rtree_new(&extents_rtree, true)) {
		return true;
	}

	if (mutex_pool_init(&extent_mutex_pool, "extent_mutex_pool",
	    WITNESS_RANK_EXTENT_POOL)) {
		return true;
	}

	if (have_dss) {
		extent_dss_boot();
	}

	return false;
}

void
extent_util_stats_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL);

	const extent_t *extent = iealloc(tsdn, ptr);
	if (unlikely(extent == NULL)) {
		*nfree = *nregs = *size = 0;
		return;
	}

	*size = extent_size_get(extent);
	if (!extent_slab_get(extent)) {
		*nfree = 0;
		*nregs = 1;
	} else {
		*nfree = extent_nfree_get(extent);
		*nregs = bin_infos[extent_szind_get(extent)].nregs;
		assert(*nfree <= *nregs);
		assert(*nfree * extent_usize_get(extent) <= *size);
	}
}

void
extent_util_stats_verbose_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size,
    size_t *bin_nfree, size_t *bin_nregs, void **slabcur_addr) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL
	    && bin_nfree != NULL && bin_nregs != NULL && slabcur_addr != NULL);

	const extent_t *extent = iealloc(tsdn, ptr);
	if (unlikely(extent == NULL)) {
		*nfree = *nregs = *size = *bin_nfree = *bin_nregs = 0;
		*slabcur_addr = NULL;
		return;
	}

	*size = extent_size_get(extent);
	if (!extent_slab_get(extent)) {
		*nfree = *bin_nfree = *bin_nregs = 0;
		*nregs = 1;
		*slabcur_addr = NULL;
		return;
	}

	*nfree = extent_nfree_get(extent);
	const szind_t szind = extent_szind_get(extent);
	*nregs = bin_infos[szind].nregs;
	assert(*nfree <= *nregs);
	assert(*nfree * extent_usize_get(extent) <= *size);

	const arena_t *arena = (arena_t *)atomic_load_p(
	    &arenas[extent_arena_ind_get(extent)], ATOMIC_RELAXED);
	assert(arena != NULL);
	const unsigned binshard = extent_binshard_get(extent);
	bin_t *bin = &arena->bins[szind].bin_shards[binshard];

	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats) {
		*bin_nregs = *nregs * bin->stats.curslabs;
		assert(*bin_nregs >= bin->stats.curregs);
		*bin_nfree = *bin_nregs - bin->stats.curregs;
	} else {
		*bin_nfree = *bin_nregs = 0;
	}
	extent_t *slab;
	if (bin->slabcur != NULL) {
		slab = bin->slabcur;
	} else {
		slab = extent_heap_first(&bin->slabs_nonfull);
	}
	*slabcur_addr = slab != NULL ? extent_addr_get(slab) : NULL;
	malloc_mutex_unlock(tsdn, &bin->lock);
}
