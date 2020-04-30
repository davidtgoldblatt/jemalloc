#ifndef JEMALLOC_INTERNAL_TCACHE_INLINES_H
#define JEMALLOC_INTERNAL_TCACHE_INLINES_H

#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/soc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/util.h"

static inline bool
tcache_enabled_get(tsd_t *tsd) {
	return tsd_tcache_enabled_get(tsd);
}

static inline void
tcache_enabled_set(tsd_t *tsd, bool enabled) {
	bool was_enabled = tsd_tcache_enabled_get(tsd);

	if (!was_enabled && enabled) {
		tsd_tcache_data_init(tsd);
	} else if (was_enabled && !enabled) {
		tcache_cleanup(tsd);
	}
	/* Commit the state last.  Above calls check current state. */
	tsd_tcache_enabled_set(tsd, enabled);
	tsd_slow_update(tsd);
}

JEMALLOC_ALWAYS_INLINE void *
tcache_alloc_small(tsd_t *tsd, arena_t *arena, tcache_t *tcache,
    size_t size, szind_t binind, bool zero, bool slow_path) {
	void *ret;
	bool tcache_success;

	assert(binind < SC_NBINS);
	cache_bin_t *bin = &tcache->bins[binind];
	ret = cache_bin_alloc(bin, &tcache_success);
	assert(tcache_success == (ret != NULL));
	if (unlikely(!tcache_success)) {
		bool tcache_hard_success;
		arena = arena_choose(tsd, arena);
		if (unlikely(arena == NULL)) {
			return NULL;
		}

		ret = tcache_alloc_small_hard(tsd_tsdn(tsd), arena, tcache,
		    bin, binind, &tcache_hard_success);
		if (tcache_hard_success == false) {
			return NULL;
		}
	}

	assert(ret);
	if (unlikely(zero)) {
		size_t usize = sz_index2size(binind);
		assert(tcache_salloc(tsd_tsdn(tsd), ret) == usize);
		memset(ret, 0, usize);
	}
	if (config_stats) {
		bin->tstats.nrequests++;
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
tcache_alloc_large(tsd_t *tsd, arena_t *arena, tcache_t *tcache, size_t size,
    szind_t szind, bool zero, bool slow_path) {
	void *ret;
	bool tcache_success;

	assert(szind >= SC_NBINS && szind < nhbins);
	cache_bin_t *bin = &tcache->bins[szind];
	ret = cache_bin_alloc(bin, &tcache_success);
	assert(tcache_success == (ret != NULL));
	if (unlikely(!tcache_success)) {
		arena = arena_choose(tsd, arena);
		ret = soc_alloc_large(tsd, &soc_global, szind, zero, arena);
	} else {
		size_t usize = sz_index2size(szind);
		memset(ret, 0, usize);
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE void
tcache_dalloc_small(tsd_t *tsd, tcache_t *tcache, void *ptr, szind_t binind,
    bool slow_path) {
	assert(tcache_salloc(tsd_tsdn(tsd), ptr)
	    <= SC_SMALL_MAXCLASS);

	cache_bin_t *bin = &tcache->bins[binind];
	if (unlikely(!cache_bin_dalloc_easy(bin, ptr))) {
		unsigned remain = cache_bin_info_ncached_max(
		    &tcache_bin_info[binind]) >> 1;
		tcache_bin_flush(tsd, tcache, bin, binind, remain);
		bool ret = cache_bin_dalloc_easy(bin, ptr);
		assert(ret);
	}
}

JEMALLOC_ALWAYS_INLINE void
tcache_dalloc_large(tsd_t *tsd, tcache_t *tcache, void *ptr, szind_t binind,
    bool slow_path) {

	assert(tcache_salloc(tsd_tsdn(tsd), ptr)
	    > SC_SMALL_MAXCLASS);
	assert(tcache_salloc(tsd_tsdn(tsd), ptr) <= tcache_maxclass);

	cache_bin_t *bin = &tcache->bins[binind];
	if (unlikely(!cache_bin_dalloc_easy(bin, ptr))) {
		unsigned remain = cache_bin_info_ncached_max(
		    &tcache_bin_info[binind]) >> 1;
		tcache_bin_flush(tsd, tcache, bin, binind, remain);
		bool ret = cache_bin_dalloc_easy(bin, ptr);
		assert(ret);
	}
}

JEMALLOC_ALWAYS_INLINE tcache_t *
tcaches_get(tsd_t *tsd, unsigned ind) {
	tcaches_t *elm = &tcaches[ind];
	if (unlikely(elm->tcache == NULL)) {
		malloc_printf("<jemalloc>: invalid tcache id (%u).\n", ind);
		abort();
	} else if (unlikely(elm->tcache == TCACHES_ELM_NEED_REINIT)) {
		elm->tcache = tcache_create_explicit(tsd);
	}
	return elm->tcache;
}

#endif /* JEMALLOC_INTERNAL_TCACHE_INLINES_H */
