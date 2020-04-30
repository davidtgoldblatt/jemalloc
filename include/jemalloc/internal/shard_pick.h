#ifndef JEMALLOC_INTERNAL_SHARD_PICK_H
#define JEMALLOC_INTERNAL_SHARD_PICK_H

#include "jemalloc/internal/prng.h"

/*
 * Quickly pick a shard from a set of N.  The idea is that you initialize a
 * shard_picker_t once per thread lifetime, and then get a consistent mapping
 * over a set of shards.
 *
 * We use the trick from
 * https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
 */

typedef struct shard_picker_s shard_picker_t;
struct shard_picker_s {
	uint32_t state;
};

static inline void
shard_picker_init(shard_picker_t *picker, uint64_t *prng_state) {
	picker->state = prng_lg_range_u64(prng_state, 32);
}

static inline uint32_t
shard_pick(shard_picker_t *picker, uint32_t nshards) {
	uint32_t ret = ((uint64_t)picker->state * (uint64_t)nshards) >> 32;
	assert(ret < nshards);
	return ret;
}

#endif /* JEMALLOC_INTERNAL_SHARD_PICK_H */
