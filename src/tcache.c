#define JEMALLOC_TCACHE_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/soc.h"

/******************************************************************************/
/* Data. */

bool	opt_tcache = true;
ssize_t	opt_lg_tcache_max = LG_TCACHE_MAXCLASS_DEFAULT;

cache_bin_info_t	*tcache_bin_info;

/* Total stack size required (per tcache).  Include the padding above. */
static size_t tcache_bin_alloc_size;
static size_t tcache_bin_alloc_alignment;

unsigned		nhbins;
size_t			tcache_maxclass;

tcaches_t		*tcaches;

/* Index of first element within tcaches that has never been used. */
static unsigned		tcaches_past;

/* Head of singly linked list tracking available tcaches elements. */
static tcaches_t	*tcaches_avail;

/* Protects tcaches{,_past,_avail}. */
static malloc_mutex_t	tcaches_mtx;

/******************************************************************************/

size_t
tcache_salloc(tsdn_t *tsdn, const void *ptr) {
	return arena_salloc(tsdn, ptr);
}

uint64_t
tcache_gc_new_event_wait(tsd_t *tsd) {
	return TCACHE_GC_INCR_BYTES;
}

uint64_t
tcache_gc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

uint64_t
tcache_gc_dalloc_new_event_wait(tsd_t *tsd) {
	return TCACHE_GC_INCR_BYTES;
}

uint64_t
tcache_gc_dalloc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

static void
tcache_event(tsd_t *tsd) {
	tcache_t *tcache = tcache_get(tsd);
	if (tcache == NULL) {
		return;
	}

	tcache_slow_t *tcache_slow = tsd_tcache_slowp_get(tsd);
	szind_t binind = tcache_slow->next_gc_bin;
	bool is_small = (binind < SC_NBINS);
	cache_bin_t *cache_bin = &tcache->bins[binind];

	cache_bin_sz_t low_water = cache_bin_low_water_get(cache_bin,
	    &tcache_bin_info[binind]);
	cache_bin_sz_t ncached = cache_bin_ncached_get(cache_bin,
	    &tcache_bin_info[binind]);
	if (low_water > 0) {
		/*
		 * Flush (ceiling) 3/4 of the objects below the low water mark.
		 */
		if (is_small) {
			assert(!tcache_slow->bin_refilled[binind]);
			tcache_bin_flush(tsd, tcache, cache_bin, binind,
			    ncached - low_water + (low_water >> 2));
			/*
			 * Reduce fill count by 2X.  Limit lg_fill_div such that
			 * the fill count is always at least 1.
			 */
			if ((cache_bin_info_ncached_max(
			    &tcache_bin_info[binind]) >>
			    (tcache_slow->lg_fill_div[binind] + 1)) >= 1) {
				tcache_slow->lg_fill_div[binind]++;
			}
		} else {
			tcache_bin_flush(tsd, tcache, cache_bin, binind,
			     ncached - low_water + (low_water >> 2));
		}
	} else if (is_small && tcache_slow->bin_refilled[binind]) {
		assert(low_water == 0);
		/*
		 * Increase fill count by 2X for small bins.  Make sure
		 * lg_fill_div stays greater than 0.
		 */
		if (tcache_slow->lg_fill_div[binind] > 1) {
			tcache_slow->lg_fill_div[binind]--;
		}
		tcache_slow->bin_refilled[binind] = false;
	}
	cache_bin_low_water_set(cache_bin);

	tcache_slow->next_gc_bin++;
	if (tcache_slow->next_gc_bin == nhbins) {
		tcache_slow->next_gc_bin = 0;
	}
}

void
tcache_gc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed == TE_INVALID_ELAPSED);
	tcache_event(tsd);
}

void
tcache_gc_dalloc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed == TE_INVALID_ELAPSED);
	tcache_event(tsd);
}

void *
tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena,
    tcache_t *tcache, cache_bin_t *cache_bin, szind_t binind,
    bool *tcache_success) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	void *ret;
	cache_bin_info_t *cache_bin_info = &tcache_bin_info[binind];

	assert(tcache_slow->arena != NULL);
	unsigned nfill = cache_bin_info_ncached_max(&tcache_bin_info[binind])
	    >> tcache_slow->lg_fill_div[binind];
	soc_cache_bin_fill_small(tsdn, &soc_global, binind, arena, cache_bin,
	    cache_bin_info, nfill);
	tcache_slow->bin_refilled[binind] = true;
	ret = cache_bin_alloc(cache_bin, tcache_success);

	return ret;
}

void
tcache_bin_flush(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t szind, unsigned rem) {
	arena_t *stats_arena = tcache->tcache_slow->arena;
	cache_bin_info_t *cache_bin_info = &tcache_bin_info[szind];
	soc_cache_bin_flush(tsd, &soc_global, szind, stats_arena, cache_bin,
	    cache_bin_info, rem);
}

void
tcache_arena_associate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena) {
	assert(tcache_slow->arena == NULL);
	tcache_slow->arena = arena;

	if (config_stats) {
		/* Link into list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);

		ql_elm_new(tcache_slow, link);
		ql_tail_insert(&arena->tcache_ql, tcache_slow, link);
		cache_bin_array_descriptor_init(
		    &tcache_slow->cache_bin_array_descriptor, tcache->bins);
		ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
		    &tcache_slow->cache_bin_array_descriptor, link);

		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
}

static void
tcache_arena_dissociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache) {
	arena_t *arena = tcache_slow->arena;
	assert(arena != NULL);
	if (config_stats) {
		/* Unlink from list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		if (config_debug) {
			bool in_ql = false;
			tcache_slow_t *iter;
			ql_foreach(iter, &arena->tcache_ql, link) {
				if (iter == tcache_slow) {
					in_ql = true;
					break;
				}
			}
			assert(in_ql);
		}
		ql_remove(&arena->tcache_ql, tcache_slow, link);
		ql_remove(&arena->cache_bin_array_descriptor_ql,
		    &tcache_slow->cache_bin_array_descriptor, link);
		tcache_stats_merge(tsdn, tcache_slow->tcache, arena);
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
	tcache_slow->arena = NULL;
}

void
tcache_arena_reassociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena) {
	tcache_arena_dissociate(tsdn, tcache_slow, tcache);
	tcache_arena_associate(tsdn, tcache_slow, tcache, arena);
}

bool
tsd_tcache_enabled_data_init(tsd_t *tsd) {
	/* Called upon tsd initialization. */
	tsd_tcache_enabled_set(tsd, opt_tcache);
	tsd_slow_update(tsd);

	if (opt_tcache) {
		/* Trigger tcache init. */
		tsd_tcache_data_init(tsd);
	}

	return false;
}

static void
tcache_init(tsd_t *tsd, tcache_slow_t *tcache_slow, tcache_t *tcache,
    void *mem) {
	tcache->tcache_slow = tcache_slow;
	tcache_slow->tcache = tcache;

	memset(&tcache_slow->link, 0, sizeof(ql_elm(tcache_t)));
	tcache_slow->next_gc_bin = 0;
	tcache_slow->arena = NULL;
	tcache_slow->dyn_alloc = mem;

	assert((TCACHE_NSLOTS_SMALL_MAX & 1U) == 0);
	memset(tcache->bins, 0, sizeof(cache_bin_t) * nhbins);

	size_t cur_offset = 0;
	cache_bin_preincrement(mem, &cur_offset);
	for (unsigned i = 0; i < nhbins; i++) {
		if (i < SC_NBINS) {
			tcache_slow->lg_fill_div[i] = 1;
			tcache_slow->bin_refilled[i] = false;
		}
		cache_bin_t *cache_bin = &tcache->bins[i];
		cache_bin_init(cache_bin, &tcache_bin_info[i], mem,
		    &cur_offset);
	}
	cache_bin_postincrement(mem, &cur_offset);
	/* Sanity check that the whole stack is used. */
	assert(cur_offset == tcache_bin_alloc_size);
}

/* Initialize auto tcache (embedded in TSD). */
bool
tsd_tcache_data_init(tsd_t *tsd) {
	tcache_slow_t *tcache_slow = tsd_tcache_slowp_get_unsafe(tsd);
	tcache_t *tcache = tsd_tcachep_get_unsafe(tsd);

	assert(cache_bin_still_zero_initialized(&tcache->bins[0]));
	size_t alignment = tcache_bin_alloc_alignment;
	size_t size = sz_sa2u(tcache_bin_alloc_size, alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, alignment, true, NULL,
	    true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return true;
	}

	tcache_init(tsd, tcache_slow, tcache, mem);
	/*
	 * Initialization is a bit tricky here.  After malloc init is done, all
	 * threads can rely on arena_choose and associate tcache accordingly.
	 * However, the thread that does actual malloc bootstrapping relies on
	 * functional tsd, and it can only rely on a0.  In that case, we
	 * associate its tcache to a0 temporarily, and later on
	 * arena_choose_hard() will re-associate properly.
	 */
	tcache_slow->arena = NULL;
	arena_t *arena;
	if (!malloc_initialized()) {
		/* If in initialization, assign to a0. */
		arena = arena_get(tsd_tsdn(tsd), 0, false);
		tcache_arena_associate(tsd_tsdn(tsd), tcache_slow, tcache,
		    arena);
	} else {
		arena = arena_choose(tsd, NULL);
		/* This may happen if thread.tcache.enabled is used. */
		if (tcache_slow->arena == NULL) {
			tcache_arena_associate(tsd_tsdn(tsd), tcache_slow,
			    tcache, arena);
		}
	}
	assert(arena == tcache_slow->arena);

	return false;
}

/* Created manual tcache for tcache.create mallctl. */
tcache_t *
tcache_create_explicit(tsd_t *tsd) {
	/*
	 * We place the cache bin stacks, then the tcache_t, then a pointer to
	 * the beginning of the whole allocation (for freeing).  The makes sure
	 * the cache bins have the requested alignment.
	 */
	size_t size = tcache_bin_alloc_size + sizeof(tcache_t)
	    + sizeof(tcache_slow_t);
	/* Naturally align the pointer stacks. */
	size = PTR_CEILING(size);
	size = sz_sa2u(size, tcache_bin_alloc_alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, tcache_bin_alloc_alignment,
	    true, NULL, true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return NULL;
	}
	tcache_t *tcache = (void *)((uintptr_t)mem + tcache_bin_alloc_size);
	tcache_slow_t *tcache_slow =
	    (void *)((uintptr_t)mem + tcache_bin_alloc_size + sizeof(tcache_t));
	tcache_init(tsd, tcache_slow, tcache, mem);

	tcache_arena_associate(tsd_tsdn(tsd), tcache_slow, tcache,
	    arena_ichoose(tsd, NULL));

	return tcache;
}

static void
tcache_flush_cache(tsd_t *tsd, tcache_t *tcache) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	assert(tcache_slow->arena != NULL);

	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		tcache_bin_flush(tsd, tcache, cache_bin, i, 0);
		if (config_stats) {
			assert(cache_bin->tstats.nrequests == 0);
		}
	}
}

void
tcache_flush(tsd_t *tsd) {
	assert(tcache_available(tsd));
	tcache_flush_cache(tsd, tsd_tcachep_get(tsd));
}

static void
tcache_destroy(tsd_t *tsd, tcache_t *tcache, bool tsd_tcache) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	tcache_flush_cache(tsd, tcache);
	arena_t *arena = tcache_slow->arena;
	tcache_arena_dissociate(tsd_tsdn(tsd), tcache_slow, tcache);

	if (tsd_tcache) {
		cache_bin_t *cache_bin = &tcache->bins[0];
		cache_bin_assert_empty(cache_bin, &tcache_bin_info[0]);
	}
	idalloctm(tsd_tsdn(tsd), tcache_slow->dyn_alloc, NULL, NULL, true,
	    true);

	/*
	 * The deallocation and tcache flush above may not trigger decay since
	 * we are on the tcache shutdown path (potentially with non-nominal
	 * tsd).  Manually trigger decay to avoid pathological cases.  Also
	 * include arena 0 because the tcache array is allocated from it.
	 */
	arena_decay(tsd_tsdn(tsd), arena_get(tsd_tsdn(tsd), 0, false),
	    false, false);

	if (arena_nthreads_get(arena, false) == 0 &&
	    !background_thread_enabled()) {
		/* Force purging when no threads assigned to the arena anymore. */
		arena_decay(tsd_tsdn(tsd), arena, false, true);
	} else {
		arena_decay(tsd_tsdn(tsd), arena, false, false);
	}
}

/* For auto tcache (embedded in TSD) only. */
void
tcache_cleanup(tsd_t *tsd) {
	tcache_t *tcache = tsd_tcachep_get(tsd);
	if (!tcache_available(tsd)) {
		assert(tsd_tcache_enabled_get(tsd) == false);
		assert(cache_bin_still_zero_initialized(&tcache->bins[0]));
		return;
	}
	assert(tsd_tcache_enabled_get(tsd));
	assert(!cache_bin_still_zero_initialized(&tcache->bins[0]));

	tcache_destroy(tsd, tcache, true);
	if (config_debug) {
		/*
		 * For debug testing only, we want to pretend we're still in the
		 * zero-initialized state.
		 */
		memset(tcache->bins, 0, sizeof(cache_bin_t) * nhbins);
	}
}

void
tcache_stats_merge(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	cassert(config_stats);

	/* Merge and reset tcache stats. */
	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		if (i < SC_NBINS) {
			unsigned binshard;
			bin_t *bin = arena_bin_choose_lock(tsdn, arena, i,
			    &binshard);
			bin->stats.nrequests += cache_bin->tstats.nrequests;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_flush_nrequests_add(tsdn,
			    &arena->stats, i, cache_bin->tstats.nrequests);
		}
		cache_bin->tstats.nrequests = 0;
	}
}

static bool
tcaches_create_prep(tsd_t *tsd, base_t *base) {
	bool err;

	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);

	if (tcaches == NULL) {
		tcaches = base_alloc(tsd_tsdn(tsd), base,
		    sizeof(tcache_t *) * (MALLOCX_TCACHE_MAX+1), CACHELINE);
		if (tcaches == NULL) {
			err = true;
			goto label_return;
		}
	}

	if (tcaches_avail == NULL && tcaches_past > MALLOCX_TCACHE_MAX) {
		err = true;
		goto label_return;
	}

	err = false;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	return err;
}

bool
tcaches_create(tsd_t *tsd, base_t *base, unsigned *r_ind) {
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);

	bool err;

	if (tcaches_create_prep(tsd, base)) {
		err = true;
		goto label_return;
	}

	tcache_t *tcache = tcache_create_explicit(tsd);
	if (tcache == NULL) {
		err = true;
		goto label_return;
	}

	tcaches_t *elm;
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcaches_avail != NULL) {
		elm = tcaches_avail;
		tcaches_avail = tcaches_avail->next;
		elm->tcache = tcache;
		*r_ind = (unsigned)(elm - tcaches);
	} else {
		elm = &tcaches[tcaches_past];
		elm->tcache = tcache;
		*r_ind = tcaches_past;
		tcaches_past++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);

	err = false;
label_return:
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);
	return err;
}

static tcache_t *
tcaches_elm_remove(tsd_t *tsd, tcaches_t *elm, bool allow_reinit) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tcaches_mtx);

	if (elm->tcache == NULL) {
		return NULL;
	}
	tcache_t *tcache = elm->tcache;
	if (allow_reinit) {
		elm->tcache = TCACHES_ELM_NEED_REINIT;
	} else {
		elm->tcache = NULL;
	}

	if (tcache == TCACHES_ELM_NEED_REINIT) {
		return NULL;
	}
	return tcache;
}

void
tcaches_flush(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcache_t *tcache = tcaches_elm_remove(tsd, &tcaches[ind], true);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		/* Destroy the tcache; recreate in tcaches_get() if needed. */
		tcache_destroy(tsd, tcache, false);
	}
}

void
tcaches_destroy(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcaches_t *elm = &tcaches[ind];
	tcache_t *tcache = tcaches_elm_remove(tsd, elm, false);
	elm->next = tcaches_avail;
	tcaches_avail = elm;
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		tcache_destroy(tsd, tcache, false);
	}
}

bool
tcache_boot(tsdn_t *tsdn, base_t *base) {
	/* If necessary, clamp opt_lg_tcache_max. */
	if (opt_lg_tcache_max < 0 || (ZU(1) << opt_lg_tcache_max) <
	    SC_SMALL_MAXCLASS) {
		tcache_maxclass = SC_SMALL_MAXCLASS;
	} else {
		tcache_maxclass = (ZU(1) << opt_lg_tcache_max);
	}

	if (malloc_mutex_init(&tcaches_mtx, "tcaches", WITNESS_RANK_TCACHES,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}

	nhbins = sz_size2index(tcache_maxclass) + 1;

	/* Initialize tcache_bin_info. */
	tcache_bin_info = (cache_bin_info_t *)base_alloc(tsdn, base,
	    nhbins * sizeof(cache_bin_info_t), CACHELINE);
	if (tcache_bin_info == NULL) {
		return true;
	}
	unsigned ncached_max;
	for (unsigned i = 0; i < SC_NBINS; i++) {
		if ((bin_infos[i].nregs << 1) <= TCACHE_NSLOTS_SMALL_MIN) {
			ncached_max = TCACHE_NSLOTS_SMALL_MIN;
		} else if ((bin_infos[i].nregs << 1) <=
		    TCACHE_NSLOTS_SMALL_MAX) {
			ncached_max = bin_infos[i].nregs << 1;
		} else {
			ncached_max = TCACHE_NSLOTS_SMALL_MAX;
		}
		cache_bin_info_init(&tcache_bin_info[i], ncached_max);
	}
	for (unsigned i = SC_NBINS; i < nhbins; i++) {
		cache_bin_info_init(&tcache_bin_info[i], TCACHE_NSLOTS_LARGE);
	}

	cache_bin_alloc_info_t alloc_info;
	cache_bin_alloc_info_init(&alloc_info);
	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_alloc_info_update(&alloc_info, &tcache_bin_info[i]);
	}
	tcache_bin_alloc_size = alloc_info.size;
	tcache_bin_alloc_alignment = alloc_info.alignment;

	return false;
}

void
tcache_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &tcaches_mtx);
}

void
tcache_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &tcaches_mtx);
}

void
tcache_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &tcaches_mtx);
}

void tcache_assert_initialized(tcache_t *tcache) {
	assert(!cache_bin_still_zero_initialized(&tcache->bins[0]));
}
