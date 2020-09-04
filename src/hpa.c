#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpa.h"

#include "jemalloc/internal/flat_bitmap.h"
#include "jemalloc/internal/witness.h"

static edata_t *hpa_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero);
static bool hpa_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero);
static bool hpa_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size);
static void hpa_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata);

bool
hpa_init(hpa_t *hpa, base_t *base, emap_t *emap, edata_cache_t *edata_cache) {
	bool err;

	err = malloc_mutex_init(&hpa->grow_mtx, "hpa_grow", WITNESS_RANK_HPA_GROW,
	    malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	err = malloc_mutex_init(&hpa->mtx, "hpa", WITNESS_RANK_HPA,
	    malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}

	hpa_central_init(&hpa->central, edata_cache, emap);
	if (err) {
		return true;
	}
	hpa->ind = base_ind_get(base);
	hpa->edata_cache = edata_cache;

	geom_grow_init(&hpa->geom_grow);

	return false;
}

bool
hpa_shard_init(hpa_shard_t *shard, hpa_t *hpa, edata_cache_t *edata_cache,
    unsigned ind, size_t ps_goal, size_t ps_alloc_max) {
	bool err;
	err = malloc_mutex_init(&shard->grow_mtx, "hpa_shard_grow",
	    WITNESS_RANK_HPA_SHARD_GROW, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	err = malloc_mutex_init(&shard->mtx, "hpa_shard",
	    WITNESS_RANK_HPA_SHARD, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}

	shard->edata_cache = edata_cache;
	shard->hpa = hpa;
	psset_init(&shard->psset);
	shard->ps_goal = ps_goal;
	shard->ps_alloc_max = ps_alloc_max;

	/*
	 * Fill these in last, so that if an hpa_shard gets used despite
	 * initialization failing, we'll at least crash instead of just
	 * operating on corrupted data.
	 */
	shard->pai.alloc = &hpa_alloc;
	shard->pai.expand = &hpa_expand;
	shard->pai.shrink = &hpa_shrink;
	shard->pai.dalloc = &hpa_dalloc;

	shard->ind = ind;
	assert(ind == base_ind_get(edata_cache->base));

	return false;
}

static edata_t *
hpa_alloc_central(tsdn_t *tsdn, hpa_shard_t *shard, size_t size_min,
    size_t size_goal) {
	bool err;
	edata_t *edata;

	hpa_t *hpa = shard->hpa;

	malloc_mutex_lock(tsdn, &hpa->mtx);
	edata = hpa_central_alloc_reuse(tsdn, &hpa->central, size_min,
	    size_goal);
	malloc_mutex_unlock(tsdn, &hpa->mtx);
	if (edata != NULL) {
		edata_arena_ind_set(edata, shard->ind);
		return edata;
	}
	/* No existing range can satisfy the request; try to grow. */
	malloc_mutex_lock(tsdn, &hpa->grow_mtx);

	/*
	 * We could have raced with other grow attempts; re-check to see if we
	 * did, and are now able to satisfy the request.
	 */
	malloc_mutex_lock(tsdn, &hpa->mtx);
	edata = hpa_central_alloc_reuse(tsdn, &hpa->central, size_min,
	    size_goal);
	malloc_mutex_unlock(tsdn, &hpa->mtx);
	if (edata != NULL) {
		malloc_mutex_unlock(tsdn, &hpa->grow_mtx);
		edata_arena_ind_set(edata, shard->ind);
		return edata;
	}

	/*
	 * No such luck. We've dropped mtx, so other allocations can proceed
	 * while we allocate the new extent.  We know no one else will grow in
	 * the meantime, though, since we still hold grow_mtx.
	 */
	size_t alloc_size;
	pszind_t skip;

	size_t hugepage_goal_min = HUGEPAGE_CEILING(size_goal);

	err = geom_grow_size_prepare(&hpa->geom_grow, hugepage_goal_min,
	    &alloc_size, &skip);
	if (err) {
		malloc_mutex_unlock(tsdn, &hpa->grow_mtx);
		return NULL;
	}

	/*
	 * Eventually, we need to think about this more systematically, and in
	 * terms of extent hooks.  For now, though, we know we only care about
	 * overcommitting systems, and we're not going to purge much.
	 */
	bool commit = true;
	void *addr = pages_map(NULL, alloc_size, HUGEPAGE, &commit);
	if (addr == NULL) {
		malloc_mutex_unlock(tsdn, &hpa->grow_mtx);
		return NULL;
	}
	pages_huge(addr, alloc_size);

	edata = edata_cache_get(tsdn, hpa->edata_cache);
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &hpa->grow_mtx);
		pages_unmap(addr, alloc_size);
		return NULL;
	}

	edata_init(edata, hpa->ind, addr, alloc_size, /* slab */ false,
	    SC_NSIZES, /* sn */ 0, extent_state_active, /* zeroed */ true,
	    /* comitted */ true, EXTENT_PAI_HPA, /* is_head */ true);

	malloc_mutex_lock(tsdn, &hpa->mtx);
	err = hpa_central_alloc_grow(tsdn, &hpa->central, size_goal, edata);
	malloc_mutex_unlock(tsdn, &hpa->mtx);

	malloc_mutex_unlock(tsdn, &hpa->grow_mtx);
	edata_arena_ind_set(edata, shard->ind);

	if (err) {
		pages_unmap(addr, alloc_size);
		edata_cache_put(tsdn, hpa->edata_cache, edata);
		return NULL;
	}
	return edata;
}

static edata_t *
hpa_alloc_psset(tsdn_t *tsdn, hpa_shard_t *shard, size_t size) {
	assert(size < shard->ps_alloc_max);

	bool err;
	edata_t *edata = edata_cache_get(tsdn, shard->edata_cache);
	edata_arena_ind_set(edata, shard->ind);

	malloc_mutex_lock(tsdn, &shard->mtx);
	err = psset_alloc_reuse(&shard->psset, edata, size);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	if (!err) {
		return edata;
	}
	/* Nothing in the psset works; we have to grow it. */
	malloc_mutex_lock(tsdn, &shard->grow_mtx);

	/* As above; check for grow races. */
	malloc_mutex_lock(tsdn, &shard->mtx);
	err = psset_alloc_reuse(&shard->psset, edata, size);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	if (!err) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return edata;
	}

	edata_t *grow_edata = hpa_alloc_central(tsdn, shard, size,
	    shard->ps_goal);
	if (grow_edata == NULL) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		edata_cache_put(tsdn, shard->edata_cache, edata);
		return NULL;
	}
	edata_arena_ind_set(grow_edata, shard->ind);
	edata_slab_set(grow_edata, true);
	fb_group_t *fb = edata_slab_data_get(grow_edata)->bitmap;
	fb_init(fb, shard->ps_goal / PAGE);

	/* We got the new edata; allocate from it. */
	malloc_mutex_lock(tsdn, &shard->mtx);
	psset_alloc_new(&shard->psset, grow_edata, edata, size);
	malloc_mutex_unlock(tsdn, &shard->mtx);

	malloc_mutex_unlock(tsdn, &shard->grow_mtx);
	return edata;
}

static edata_t *
hpa_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero) {
	assert((size & PAGE_MASK) == 0);
	/* We don't handle alignment or zeroing for now. */
	if (alignment > PAGE) {
		return NULL;
	}
	if (zero) {
		return NULL;
	}

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	hpa_shard_t *shard = (hpa_shard_t *)self;

	edata_t *edata;
	if (size <= shard->ps_alloc_max) {
		edata = hpa_alloc_psset(tsdn, shard, size);
		emap_register_boundary(tsdn, shard->hpa->central.emap, edata,
		    SC_NSIZES, /* slab */ false);
	} else {
		edata = hpa_alloc_central(tsdn, shard, size, size);
	}

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	if (edata != NULL) {
		emap_assert_mapped(tsdn, shard->hpa->central.emap, edata);
		assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
		assert(edata_state_get(edata) == extent_state_active);
		assert(edata_arena_ind_get(edata) == shard->ind);
		assert(edata_szind_get_maybe_invalid(edata) == SC_NSIZES);
		assert(!edata_slab_get(edata));
		assert(edata_committed_get(edata));
		assert(edata_base_get(edata) == edata_addr_get(edata));
		assert(edata_base_get(edata) != NULL);
	}
	return edata;
}

static bool
hpa_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero) {
	/* Expand not yet supported. */
	return true;
}

static bool
hpa_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size) {
	/* Shrink not yet supported. */
	return true;
}

static void
hpa_dalloc_central(tsdn_t *tsdn, hpa_shard_t *shard, edata_t *edata) {
	hpa_t *hpa = shard->hpa;

	edata_arena_ind_set(edata, hpa->ind);
	malloc_mutex_lock(tsdn, &hpa->mtx);
	hpa_central_dalloc(tsdn, &hpa->central, edata);
	malloc_mutex_unlock(tsdn, &hpa->mtx);
}

static void
hpa_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata) {
	hpa_shard_t *shard = (hpa_shard_t *)self;

	edata_addr_set(edata, edata_base_get(edata));
	edata_zeroed_set(edata, false);

	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_state_get(edata) == extent_state_active);
	assert(edata_arena_ind_get(edata) == shard->ind);
	assert(edata_szind_get_maybe_invalid(edata) == SC_NSIZES);
	assert(!edata_slab_get(edata));
	assert(edata_committed_get(edata));
	assert(edata_base_get(edata) != NULL);

	if (edata_ps_get(edata) != NULL) {
		emap_deregister_boundary(tsdn, shard->hpa->central.emap, edata);

		malloc_mutex_lock(tsdn, &shard->mtx);
		edata_t *evicted_ps = psset_dalloc(&shard->psset, edata);
		malloc_mutex_unlock(tsdn, &shard->mtx);

		edata_cache_put(tsdn, shard->edata_cache, edata);

		if (evicted_ps != NULL) {
			bool err = emap_register_boundary(tsdn,
			    shard->hpa->central.emap, evicted_ps, SC_NSIZES,
			    /* slab */ false);
			/*
			 * Registration can only fail on OOM, but the boundary
			 * mappings should have been initialized during
			 * allocation.
			 */
			assert(!err);
			edata_slab_set(evicted_ps, false);
			edata_ps_set(evicted_ps, NULL);

			assert(edata_arena_ind_get(evicted_ps) == shard->ind);
			hpa_dalloc_central(tsdn, shard, evicted_ps);
		}
	} else {
		hpa_dalloc_central(tsdn, shard, edata);
	}
}

void
hpa_shard_destroy(tsdn_t *tsdn, hpa_shard_t *shard) {
	/*
	 * By the time we're here, the arena code should have dalloc'd all the
	 * active extents, which means we should have eventually evicted
	 * everything from the psset, so it shouldn't be able to serve even a
	 * 1-page allocation.
	 */
	if (config_debug) {
		edata_t edata = {0};
		malloc_mutex_lock(tsdn, &shard->mtx);
		bool psset_empty = psset_alloc_reuse(&shard->psset, &edata,
		    PAGE);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		assert(psset_empty);
	}
}

void
hpa_shard_prefork2(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->grow_mtx);
}

void
hpa_shard_prefork3(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->mtx);
}

void
hpa_shard_postfork_parent(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_postfork_parent(tsdn, &shard->grow_mtx);
	malloc_mutex_postfork_parent(tsdn, &shard->mtx);
}

void
hpa_shard_postfork_child(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_postfork_child(tsdn, &shard->grow_mtx);
	malloc_mutex_postfork_child(tsdn, &shard->mtx);
}

void
hpa_prefork3(tsdn_t *tsdn, hpa_t *hpa) {
	malloc_mutex_prefork(tsdn, &hpa->grow_mtx);
	malloc_mutex_prefork(tsdn, &hpa->mtx);
}

void
hpa_postfork_parent(tsdn_t *tsdn, hpa_t *hpa) {
	malloc_mutex_postfork_parent(tsdn, &hpa->grow_mtx);
	malloc_mutex_postfork_parent(tsdn, &hpa->mtx);
}

void
hpa_postfork_child(tsdn_t *tsdn, hpa_t *hpa) {
	malloc_mutex_postfork_child(tsdn, &hpa->grow_mtx);
	malloc_mutex_postfork_child(tsdn, &hpa->mtx);
}
