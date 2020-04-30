#include "test/jemalloc_test.h"

#include "jemalloc/internal/shard_pick.h"

TEST_BEGIN(test_consistent) {
	uint64_t prng_state = 0x123456789ABCDEF0ULL;
	for (int i = 0; i < 1000; i++) {
		shard_picker_t picker;
		shard_picker_init(&picker, &prng_state);
		for (int nshards = 1; nshards < 100; nshards++) {
			uint32_t shard = shard_pick(&picker, nshards);
			for (int j = 0; j < 10; j++) {
				uint32_t shard2 = shard_pick(&picker, nshards);
				expect_u32_eq(shard, shard2,
				    "Shard choice not consistent.");
			}
		}
	}
}
TEST_END

TEST_BEGIN(test_bounded) {
	const int npicks = 10*1000;
	const int shards_max = 10*1000;

	uint64_t prng_state = 0x123456789ABCDEF0ULL;
	for (int i = 0; i < npicks; i++) {
		shard_picker_t picker;
		shard_picker_init(&picker, &prng_state);
		for (int nshards = 1; nshards < shards_max; nshards++) {
			uint32_t shard = shard_pick(&picker, nshards);
			expect_u32_lt(shard, nshards,
			    "Chose an out-of-range shard.");
		}
	}
}
TEST_END

static void
run_distribution_test(uint32_t nshards, uint64_t *prng_state) {
	/*
	 * We rely a little bit on the caller choosing reasonable enough shard
	 * counts to make failures unlikely.
	 */
	const uint64_t picks_per_shard = 10 * 1000;
	const uint64_t picks_min = 9 * 1000;
	const uint64_t picks_max = 11 * 1000;

	uint64_t *shard_picks = calloc(nshards, sizeof(uint64_t));
	for (uint64_t i = 0; i < picks_per_shard * nshards; i++) {
		shard_picker_t picker;
		shard_picker_init(&picker, prng_state);
		uint32_t shard = shard_pick(&picker, nshards);
		assert_u32_lt(shard, nshards, "Out-of-bounds shard");
		shard_picks[shard]++;
	}

	for (uint32_t i = 0; i < nshards; i++) {
		expect_u32_gt(shard_picks[i], picks_min,
		    "Unbalanced shard selections.");
		expect_u32_lt(shard_picks[i], picks_max,
		    "Unbalanced shard selections.");
	}

	free(shard_picks);
}

TEST_BEGIN(test_well_distributed) {
	uint64_t prng_state = 0x123456789ABCDEF0ULL;
	for (uint32_t nshards = 1; nshards < 100; nshards++) {
		run_distribution_test(nshards, &prng_state);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_consistent,
	    test_bounded,
	    test_well_distributed);
}
