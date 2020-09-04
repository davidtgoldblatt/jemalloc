#ifndef JEMALLOC_INTERNAL_HPA_H
#define JEMALLOC_INTERNAL_HPA_H

#include "jemalloc/internal/geom_grow.h"
#include "jemalloc/internal/hpa_central.h"
#include "jemalloc/internal/pai.h"
#include "jemalloc/internal/psset.h"

typedef struct hpa_s hpa_t;
struct hpa_s {
	malloc_mutex_t grow_mtx;
	malloc_mutex_t mtx;
	hpa_central_t central;
	/* The arena ind we're associated with. */
	unsigned ind;
	edata_cache_t *edata_cache;
	geom_grow_t geom_grow;
};

/* Used only by CTL; not actually stored here (i.e., all derived). */
typedef struct hpa_shard_stats_s hpa_shard_stats_t;
struct hpa_shard_stats_s {
	psset_bin_stats_t psset_fullstats;
	psset_bin_stats_t psset_stats[PSSET_NPSIZES];
};

typedef struct hpa_shard_s hpa_shard_t;
struct hpa_shard_s {
	/*
	 * pai must be the first member; we cast from a pointer to it to a
	 * pointer to the hpa_shard_t.
	 */
	pai_t pai;
	malloc_mutex_t grow_mtx;
	malloc_mutex_t mtx;
	edata_cache_t *edata_cache;
	hpa_t *hpa;
	psset_t psset;

	/*
	 * When we're grabbing a new ps from the central allocator, how big
	 * would we like it to be?
	 */
	size_t ps_goal;
	/* What's the maximum size we'll try to allocate out of the psset? */
	size_t ps_alloc_max;
	/* The arena ind we're associated with. */
	unsigned ind;
};

bool hpa_init(hpa_t *hpa, base_t *base, emap_t *emap,
    edata_cache_t *edata_cache);
bool hpa_shard_init(hpa_shard_t *shard, hpa_t *hpa,
    edata_cache_t *edata_cache, unsigned ind, size_t ps_goal,
    size_t ps_alloc_max);
void hpa_shard_destroy(tsdn_t *tsdn, hpa_shard_t *shard);

/*
 * We share the fork ordering with the PA and arena prefork handling; that's why
 * these are 2 and 3 rather than 0 or 1.
 */
void hpa_shard_prefork2(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_prefork3(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_postfork_parent(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_postfork_child(tsdn_t *tsdn, hpa_shard_t *shard);

/*
 * These should be acquired after all the shard locks in phase 4, but before any
 * locks in phase 4.  The central HPA may acquire an edata cache mutex (of a0),
 * so it needs to be lower in the witness ordering, but it's also logically
 * global and not tied to any particular arena.
 */
void hpa_prefork3(tsdn_t *tsdn, hpa_t *hpa);
void hpa_postfork_parent(tsdn_t *tsdn, hpa_t *hpa);
void hpa_postfork_child(tsdn_t *tsdn, hpa_t *hpa);

#endif /* JEMALLOC_INTERNAL_HPA_H */
