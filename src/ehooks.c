#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/extent_mmap.h"

/*
 * Some users rely on a pattern where they mostly use their custom hooks, but
 * tweak or wrap one or two.  To avoid breaking them while we iterate, we use
 * the value 1 as a marker, and detect it explicitly.
 */
const extent_hooks_t extent_hooks_default = {
	(extent_alloc_t *)1,
	(extent_dalloc_t *)1,
	(extent_destroy_t *)1,
	(extent_commit_t *)1,
	(extent_decommit_t *)1,
	(extent_purge_t *)1,
	(extent_purge_t *)1,
	(extent_split_t *)1,
	(extent_merge_t *)1
};

static void *
ehooks_alloc_default_core(tsdn_t *tsdn, arena_t *arena, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    dss_prec_t dss_prec) {
	void *ret;

	assert(size != 0);
	assert(alignment != 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL) {
		return ret;
	}
	/* mmap. */
	if ((ret = extent_alloc_mmap(new_addr, size, alignment, zero, commit))
	    != NULL) {
		return ret;
	}
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL) {
		return ret;
	}

	/* All strategies for allocation failed. */
	return NULL;
}

void *
ehooks_alloc_default_impl(tsdn_t *tsdn, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec,
    unsigned arena_ind) {
	/*
	 * This is a temporary hack; eventually, we'll add support for all
	 * arenas to over-allocate.  In the short term, though, we only support
	 * it for the extent_dss pathways (since it needs some special support,
	 * like grabbing an extra extent_t for the unused gap).  So we forget
	 * the arena pointer and then reload it.
	 */
	arena_t *arena = arena_get(tsdn, arena_ind, false);
	assert(arena != NULL);

	void *ret = ehooks_alloc_default_core(tsdn, arena, new_addr, size,
	    alignment, zero, commit, dss_prec);

	if (have_madvise_huge && ret != NULL) {
		pages_set_thp_state(ret, size);
	}
	return ret;
}

bool
ehooks_dalloc_default_impl(void *addr, size_t size) {
	if (!have_dss || !extent_in_dss(addr)) {
		return extent_dalloc_mmap(addr, size);
	}
	return true;
}

void
ehooks_destroy_default_impl(void *addr, size_t size) {
	if (!have_dss || !extent_in_dss(addr)) {
		pages_unmap(addr, size);
	}
}

bool
ehooks_commit_default_impl(void *addr, size_t size) {
	return pages_commit(addr, size);
}

bool
ehooks_decommit_default_impl(void *addr, size_t size) {
	return pages_decommit(addr, size);
}

bool
ehooks_purge_lazy_default_impl(void *addr, size_t size) {
	assert(addr != NULL);
	assert(((uintptr_t)addr & PAGE_MASK) == 0);
	assert((size & PAGE_MASK) == 0);
#ifdef PAGES_CAN_PURGE_LAZY
	return pages_purge_lazy(addr, size);
#else
	return true;
#endif
}

bool
ehooks_purge_forced_default_impl(void *addr, size_t size) {
	assert(addr != NULL);
	assert(((uintptr_t)addr & PAGE_MASK) == 0);
	assert((size & PAGE_MASK) == 0);
#ifdef PAGES_CAN_PURGE_FORCED
	return pages_purge_forced(addr, size);
#else
	return true;
#endif
}

bool
ehooks_split_default_impl() {
	if (maps_coalesce) {
		return false;
	}
	if (opt_retain) {
		/*
		 * Without retain, only whole regions can be purged (required by
		 * MEM_RELEASE on Windows) -- therefore disallow splitting.  See
		 * comments in extent_head_no_merge().
		 */
		return false;
	}
	return true;
}

bool
ehooks_merge_default_impl(void *addr_a, void *addr_b) {
	if (!maps_coalesce && !opt_retain) {
		/* See comment in split, above */
		return true;
	}
	if (have_dss && !extent_dss_mergeable(addr_a, addr_b)) {
		return true;
	}

	return false;
}
