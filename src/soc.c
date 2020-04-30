#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/soc.h"

soc_t soc_global;
uint32_t soc_tiny_shards;
size_t soc_tiny_bytes;
uint32_t soc_small_shards;
size_t soc_small_bytes;
uint32_t soc_medium_shards;
size_t soc_medium_bytes;
uint32_t soc_large_shards;
size_t soc_large_bytes;

static inline soc_bin_t *
soc_pick_bin(soc_t *soc, szind_t szind, shard_picker_t *picker) {
	if (szind >= soc->nbinsets) {
		return NULL;
	}
	soc_binset_t *binset = &soc->binsets[szind];
	uint32_t shard = shard_pick(picker, binset->nbins);
	return &binset->bins[shard];
}

void
soc_cache_bin_fill_small(tsdn_t *tsdn, soc_t *soc, szind_t szind,
    arena_t *arena, cache_bin_t *cache_bin, cache_bin_info_t *cache_bin_info,
    cache_bin_sz_t nfill) {
	cache_bin_assert_empty(cache_bin, cache_bin_info);
	if (tsdn_null(tsdn)) {
		arena_cache_bin_fill_small(tsdn, arena, cache_bin,
		    cache_bin_info, szind, nfill);
		return;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	soc_bin_t *soc_bin = soc_pick_bin(soc, szind,
	    tsd_shard_pickerp_get(tsd));
	if (soc_bin == NULL) {
		arena_cache_bin_fill_small(tsdn, arena, cache_bin,
		    cache_bin_info, szind, nfill);
		return;
	}
	cache_bin_t *soc_cache_bin = &soc_bin->cache_bin;
	cache_bin_info_t *soc_cache_bin_info =
	    &soc->binsets[szind].cache_bin_info;

	/*
	 * If we can't hope to satisfy the request, even after getting more
	 * items, don't bother trying.
	 */
	if (nfill > cache_bin_info_ncached_max(soc_cache_bin_info)) {
		arena_cache_bin_fill_small(tsdn, arena, cache_bin,
		    cache_bin_info, szind, nfill);
		return;
	}

	malloc_mutex_lock(tsdn, &soc_bin->mtx);

	/*
	 * If we touch the arena, it will be through the soc_cache_bin; it's now
	 * our job to update stats.
	 */
	soc_cache_bin->tstats.nrequests += cache_bin->tstats.nrequests;
	cache_bin->tstats.nrequests = 0;

	/*
	 * We use size_ts here, since intermediate computations can overflow a
	 * cache_bin_sz_t.
	 */

	size_t cur = cache_bin_ncached_get(soc_cache_bin, soc_cache_bin_info);
	if (cur < nfill) {
		/*
		 * Fill what we can, then refill into the (now empty) SOC cache
		 * bin.
		 */
		cache_bin_to_cache_bin_fill(cache_bin, cache_bin_info,
		    soc_cache_bin, soc_cache_bin_info, cur);
		nfill -= cur;

		/*
		 * We'll try to leave the soc cache bin half-full after the
		 * refill.
		 */
		size_t goal_size = nfill + cache_bin_info_ncached_max(
		    soc_cache_bin_info) / 2;
		if (goal_size > cache_bin_info_ncached_max(
		    soc_cache_bin_info)) {
			goal_size = cache_bin_info_ncached_max(
			    soc_cache_bin_info);
		}
		arena_cache_bin_fill_small(tsdn, arena, soc_cache_bin,
		    soc_cache_bin_info, szind, nfill);
		cur = cache_bin_ncached_get(soc_cache_bin, soc_cache_bin_info);
		if (cur < nfill) {
			/*
			 * We OOMed in the arena fill attempt.  Give the caller
			 * as much as we can.
			 */
			nfill = cur;
		}
	}

	cache_bin_to_cache_bin_fill(cache_bin, cache_bin_info, soc_cache_bin,
	    soc_cache_bin_info, nfill);

	malloc_mutex_unlock(tsdn, &soc_bin->mtx);
}

void *
soc_alloc_large(tsd_t *tsd, soc_t *soc, szind_t szind, bool zero,
    arena_t *arena) {
	tsdn_t *tsdn = tsd_tsdn(tsd);
	soc_bin_t *soc_bin = soc_pick_bin(soc, szind,
	    tsd_shard_pickerp_get(tsd));
	if (soc_bin == NULL) {
		return large_malloc(tsdn, arena, sz_index2size(szind), zero);
	}
	cache_bin_t *soc_cache_bin = &soc_bin->cache_bin;

	malloc_mutex_lock(tsdn, &soc_bin->mtx);
	bool success;
	void *ret = cache_bin_alloc(soc_cache_bin, &success);
	if (likely(success)) {
		soc_cache_bin->tstats.nrequests++;
	}
	malloc_mutex_unlock(tsdn, &soc_bin->mtx);

	if (likely(success)) {
		if (unlikely(zero)) {
			size_t usize = sz_index2size(szind);
			memset(ret, 0, usize);
		}
		return ret;
	} else {
		return large_malloc(tsdn, arena, sz_index2size(szind), zero);
	}
}

void
soc_cache_bin_flush(tsd_t *tsd, soc_t *soc, szind_t szind,
    arena_t *stats_arena, cache_bin_t *cache_bin,
    cache_bin_info_t *cache_bin_info, unsigned rem) {
	/* Just for convenience. */
	tsdn_t *tsdn = tsd_tsdn(tsd);

	size_t ncur = cache_bin_ncached_get(cache_bin, cache_bin_info);
	size_t nflush = ncur - rem;

	soc_bin_t *soc_bin = soc_pick_bin(soc, szind,
	    tsd_shard_pickerp_get(tsd));
	if (soc_bin == NULL) {
		arena_cache_bin_flush(tsd, cache_bin, cache_bin_info, szind,
		    rem, stats_arena);
		return;
	}
	cache_bin_t *soc_cache_bin = &soc_bin->cache_bin;
	cache_bin_info_t *soc_cache_bin_info =
	    &soc->binsets[szind].cache_bin_info;

	size_t soc_max = cache_bin_info_ncached_max(soc_cache_bin_info);

	/* As in fill - go straight to the arena if we're useless. */
	if (nflush > soc_max) {
		arena_cache_bin_flush(tsd, cache_bin, cache_bin_info, szind,
		    rem, stats_arena);
		return;
	}
	malloc_mutex_lock(tsdn, &soc_bin->mtx);

	size_t soc_ncur = cache_bin_ncached_get(soc_cache_bin,
	    soc_cache_bin_info);

	/* As in fill, update nrequests if necessary. */
	soc_cache_bin->tstats.nrequests += cache_bin->tstats.nrequests;
	cache_bin->tstats.nrequests = 0;

	/*
	 * We might go negative in intermediate computations -- use ssize_t to
	 * make things easy.
	 */
	if (soc_ncur + nflush > soc_max) {
		/*
		 * We need to flush some number of items.  Our goal is to be
		 * half-full at the end of everything, as in fill.
		 */
		size_t soc_nflush = soc_ncur + nflush - soc_max / 2;

		unsigned soc_rem;
		if (soc_nflush > soc_ncur) {
			soc_rem = 0;
		} else {
			soc_rem = (unsigned)(soc_ncur - soc_nflush);
		}
		arena_cache_bin_flush(tsd, soc_cache_bin, soc_cache_bin_info,
		    szind, soc_rem, stats_arena);
	}
	cache_bin_to_cache_bin_flush(soc_cache_bin, soc_cache_bin_info,
	    cache_bin, cache_bin_info, nflush);
	malloc_mutex_unlock(tsdn, &soc_bin->mtx);
}

bool
soc_init(tsdn_t *tsdn, soc_t *soc, soc_binset_info_t *infos, szind_t ninfos,
    base_t *base) {
	assert(ninfos < SC_NSIZES);
	cache_bin_alloc_info_t alloc_info;
	cache_bin_alloc_info_init(&alloc_info);
	size_t nbins_total = 0;
	/* Get the size needed for the bins. */
	for (szind_t i = 0; i < ninfos; i++) {
		cache_bin_info_init(&soc->binsets[i].cache_bin_info,
		    infos[i].max_per_bin);
		for (size_t j = 0; j < infos[i].nbins; j++) {
			nbins_total++;
			cache_bin_alloc_info_update(&alloc_info,
			    &soc->binsets[i].cache_bin_info);
		}
	}
	size_t cache_bin_size = alloc_info.size;
	size_t soc_bin_size = nbins_total * sizeof(soc_bin_t);
	void *mem = base_alloc(tsdn, base, cache_bin_size + soc_bin_size,
	    alloc_info.alignment);
	if (mem == NULL) {
		soc->nbinsets = 0;
		return true;
	}
	soc_bin_t *soc_bins = (soc_bin_t *)((uintptr_t)mem + cache_bin_size);
	size_t cache_bin_offset = 0;
	cache_bin_preincrement(mem, &cache_bin_offset);

	size_t cur_bin = 0;
	for (szind_t i = 0; i < ninfos; i++) {
		/*
		 * Recall that we initialized the binset cache_bin_infos above,
		 * but nothing else.
		 */
		soc_binset_t *soc_binset = &soc->binsets[i];
		soc_binset->bins = &soc_bins[cur_bin];
		soc_binset->nbins = infos[i].nbins;

		for (size_t j = 0; j < infos[i].nbins; j++) {
			soc_bin_t *soc_bin = &soc_bins[cur_bin];
			bool err = malloc_mutex_init(&soc_bin->mtx, "soc_bin",
			    WITNESS_RANK_SOC_BIN, malloc_mutex_rank_exclusive);
			if (err) {
				soc->nbinsets = 0;
				return true;
			}
			cache_bin_init(&soc_bin->cache_bin,
			    &soc_binset->cache_bin_info, mem,
			    &cache_bin_offset);
			cur_bin++;
		}
	}
	cache_bin_postincrement(mem, &cache_bin_offset);
	assert(cache_bin_offset == cache_bin_size);
	assert(cur_bin == nbins_total);
	soc->nbinsets = ninfos;
	return false;
}

void
soc_prefork(tsdn_t *tsdn, soc_t *soc) {
	for (szind_t i = 0; i < soc->nbinsets; i++) {
		soc_binset_t *binset = &soc->binsets[i];
		for (size_t j = 0; j < binset->nbins; j++) {
			malloc_mutex_prefork(tsdn, &binset->bins[j].mtx);
		}
	}
}

void
soc_postfork_parent(tsdn_t *tsdn, soc_t *soc) {
	for (szind_t i = 0; i < soc->nbinsets; i++) {
		soc_binset_t *binset = &soc->binsets[i];
		for (size_t j = 0; j < binset->nbins; j++) {
			malloc_mutex_postfork_parent(tsdn,
			    &binset->bins[j].mtx);
		}
	}
}

void
soc_postfork_child(tsdn_t *tsdn, soc_t *soc) {
	for (szind_t i = 0; i < soc->nbinsets; i++) {
		soc_binset_t *binset = &soc->binsets[i];
		for (size_t j = 0; j < binset->nbins; j++) {
			malloc_mutex_postfork_child(tsdn,
			    &binset->bins[j].mtx);
		}
	}
}

void
soc_boot() {
	soc_binset_info_t infos[SC_NSIZES];
	szind_t ninfos = 0;
	for (szind_t i = 0; i < SC_NSIZES; i++) {
		ninfos = i;
		uint32_t shards;
		size_t bytes;
		size_t size = sz_index2size(i);
		if (size <= 128) {
			shards = soc_tiny_shards;
			bytes = soc_tiny_bytes;
		} else if (size <= 1024) {
			shards = soc_small_shards;
			bytes = soc_small_bytes;
		} else if (size <= 8 * 1024) {
			shards = soc_medium_shards;
			bytes = soc_medium_bytes;
		} else if (size <= 64 * 1024) {
			shards = soc_large_shards;
			bytes = soc_large_bytes;
		} else {
			break;
		}
		if (bytes == 0 || shards == 0) {
			break;
		}
		size_t bytes_per_shard = bytes / shards;
		size_t objects_per_shard = bytes_per_shard / size;
		/*
		 * We fill up to half the max capacity in the shard.  That
		 * capacity must therefore be at least 2.
		 */
		if (objects_per_shard < 2) {
			break;
		}
		/* Let's also not get anything crazy large. */
		if (objects_per_shard > 4 * 1024) {
			objects_per_shard = 4 * 1024;
		}
		infos[i].max_per_bin = objects_per_shard;
		infos[i].nbins = shards;
	}
	if (ninfos != 0) {
		bool err = soc_init(TSDN_NULL, &soc_global, infos, ninfos,
		    b0get());
		/*
		 * The initialization fails safely, in the sense of setting
		 * nbinsets to 0.  We can ignore it.
		 */
		(void)err;
	}
}
