#ifndef JEMALLOC_INTERNAL_SOC_H
#define JEMALLOC_INTERNAL_SOC_H

#include "jemalloc/internal/shard_pick.h"

/* A small object cache. */

/* Used to size the binset. */
typedef struct soc_binset_info_s soc_binset_info_t;
struct soc_binset_info_s {
	size_t max_per_bin;
	uint32_t nbins;
};


/* A soc bin holds objects of a particular size. */
typedef struct soc_bin_s soc_bin_t;
struct soc_bin_s {
	malloc_mutex_t mtx;
	cache_bin_t cache_bin;
};

/* A collection of bins, all of the same size. */
typedef struct soc_binset_s soc_binset_t;
struct soc_binset_s {
	soc_bin_t *bins;
	uint32_t nbins;
	cache_bin_info_t cache_bin_info;
};

typedef struct soc_s soc_t;
struct soc_s {
	soc_binset_t binsets[SC_NSIZES];
	szind_t nbinsets;
};

/*
 * Tiny: <= 128 (9 size classes)
 * Small: <= 1k  (12 size classes)
 * Medium: <= 8k (12 size classes)
 * Large: <= 64k (12 size classes)
 */
extern soc_t soc_global;
extern uint32_t soc_tiny_shards;
extern size_t soc_tiny_bytes;
extern uint32_t soc_small_shards;
extern size_t soc_small_bytes;
extern uint32_t soc_medium_shards;
extern size_t soc_medium_bytes;
extern uint32_t soc_large_shards;
extern size_t soc_large_bytes;

static inline bool
soc_global_enabled() {
	return soc_tiny_shards != 0 || soc_tiny_bytes != 0
	    || soc_small_shards != 0 || soc_small_bytes != 0
	    || soc_medium_shards != 0 || soc_medium_bytes != 0
	    || soc_large_shards != 0 || soc_large_bytes != 0;
}

void soc_cache_bin_fill_small(tsdn_t *tsdn, soc_t *soc, szind_t szind,
    arena_t *arena, cache_bin_t *cache_bin, cache_bin_info_t *cache_bin_info,
    cache_bin_sz_t nfill);
void *soc_alloc_large(tsd_t *tsd, soc_t *soc, szind_t szind, bool zero,
    arena_t *arena);
void soc_cache_bin_flush(tsd_t *tsd, soc_t *soc, szind_t szind,
    arena_t *stats_arena, cache_bin_t *cache_bin,
    cache_bin_info_t *cache_bin_info, unsigned rem);

void soc_boot();
bool soc_init(tsdn_t *tsdn, soc_t *soc, soc_binset_info_t *infos,
    szind_t ninfos, base_t *base);
void soc_prefork(tsdn_t *tsdn, soc_t *soc);
void soc_postfork_parent(tsdn_t *tsdn, soc_t *soc);
void soc_postfork_child(tsdn_t *tsdn, soc_t *soc);

#endif /* JEMALLOC_INTERNAL_SOC_H */
