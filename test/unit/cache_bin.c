#include "test/jemalloc_test.h"

static void
cache_bin_create(cache_bin_t *bin, cache_bin_info_t *info, void **to_free,
    size_t nslots) {
	cache_bin_info_init(info, nslots);

	cache_bin_alloc_info_t alloc_info;
	cache_bin_alloc_info_init(&alloc_info);
	cache_bin_alloc_info_update(&alloc_info, info);
	*to_free = mallocx(alloc_info.size,
	    MALLOCX_ALIGN(alloc_info.alignment));
	assert_ptr_not_null(*to_free, "Unexpected mallocx failure");

	size_t cur_offset = 0;
	cache_bin_preincrement(*to_free, &cur_offset);
	cache_bin_init(bin, info, *to_free, &cur_offset);
	cache_bin_postincrement(*to_free, &cur_offset);

	assert_zu_eq(cur_offset, alloc_info.size,
	    "Should use all requested memory");
}

static void
do_fill_test(cache_bin_t *bin, cache_bin_info_t *info, void **ptrs,
    cache_bin_sz_t ncached_max, cache_bin_sz_t nfill_attempt,
    cache_bin_sz_t nfill_succeed) {
	bool success;
	void *ptr;
	assert_true(cache_bin_ncached_get(bin, info) == 0, "");
	CACHE_BIN_PTR_ARRAY_DECLARE(arr, nfill_attempt);
	cache_bin_init_ptr_array_for_fill(bin, info, &arr, nfill_attempt);
	for (cache_bin_sz_t i = 0; i < nfill_succeed; i++) {
		arr.ptr[i] = &ptrs[i];
	}
	cache_bin_finish_fill(bin, info, &arr, nfill_succeed);
	expect_true(cache_bin_ncached_get(bin, info) == nfill_succeed, "");
	cache_bin_low_water_set(bin);

	for (cache_bin_sz_t i = 0; i < nfill_succeed; i++) {
		ptr = cache_bin_alloc(bin, &success);
		expect_true(success, "");
		expect_ptr_eq(ptr, (void *)&ptrs[i],
		    "Should pop in order filled");
		expect_true(cache_bin_low_water_get(bin, info)
		    == nfill_succeed - i - 1, "");
	}
	expect_true(cache_bin_ncached_get(bin, info) == 0, "");
	expect_true(cache_bin_low_water_get(bin, info) == 0, "");
}

static void
do_flush_test(cache_bin_t *bin, cache_bin_info_t *info, void **ptrs,
    cache_bin_sz_t nfill, cache_bin_sz_t nflush) {
	bool success;
	assert_true(cache_bin_ncached_get(bin, info) == 0, "");

	for (cache_bin_sz_t i = 0; i < nfill; i++) {
		success = cache_bin_dalloc_easy(bin, &ptrs[i]);
		expect_true(success, "");
	}

	CACHE_BIN_PTR_ARRAY_DECLARE(arr, nflush);
	cache_bin_init_ptr_array_for_flush(bin, info, &arr, nflush);
	for (cache_bin_sz_t i = 0; i < nflush; i++) {
		expect_ptr_eq(cache_bin_ptr_array_get(&arr, i), &ptrs[i], "");
	}
	cache_bin_finish_flush(bin, info, &arr, nflush);

	expect_true(cache_bin_ncached_get(bin, info) == nfill - nflush, "");
	while (cache_bin_ncached_get(bin, info) > 0) {
		cache_bin_alloc(bin, &success);
	}
}

TEST_BEGIN(test_cache_bin) {
	const size_t nslots = 100;
	bool success;
	void *ptr;

	void *to_free = NULL;
	cache_bin_t bin;
	cache_bin_info_t info;
	cache_bin_create(&bin, &info, &to_free, nslots);

	/* Initialize to empty; should then have 0 elements. */
	cache_bin_sz_t ncached_max = cache_bin_info_ncached_max(&info);
	expect_true(cache_bin_ncached_get(&bin, &info) == 0, "");
	expect_true(cache_bin_low_water_get(&bin, &info) == 0, "");

	ptr = cache_bin_alloc_easy(&bin, &success);
	expect_false(success, "Shouldn't successfully allocate when empty");
	expect_ptr_null(ptr, "Shouldn't get a non-null pointer on failure");

	ptr = cache_bin_alloc(&bin, &success);
	expect_false(success, "Shouldn't successfully allocate when empty");
	expect_ptr_null(ptr, "Shouldn't get a non-null pointer on failure");

	/*
	 * We allocate one more item than ncached_max, so we can test cache bin
	 * exhaustion.
	 */
	void **ptrs = mallocx(sizeof(void *) * (ncached_max + 1), 0);
	assert_ptr_not_null(ptrs, "Unexpected mallocx failure");
	for  (cache_bin_sz_t i = 0; i < ncached_max; i++) {
		expect_true(cache_bin_ncached_get(&bin, &info) == i, "");
		success = cache_bin_dalloc_easy(&bin, &ptrs[i]);
		expect_true(success,
		    "Should be able to dalloc into a non-full cache bin.");
		expect_true(cache_bin_low_water_get(&bin, &info) == 0,
		    "Pushes and pops shouldn't change low water of zero.");
	}
	expect_true(cache_bin_ncached_get(&bin, &info) == ncached_max, "");
	success = cache_bin_dalloc_easy(&bin, &ptrs[ncached_max]);
	expect_false(success, "Shouldn't be able to dalloc into a full bin.");

	cache_bin_low_water_set(&bin);

	for (cache_bin_sz_t i = 0; i < ncached_max; i++) {
		expect_true(cache_bin_low_water_get(&bin, &info)
		    == ncached_max - i, "");
		expect_true(cache_bin_ncached_get(&bin, &info)
		    == ncached_max - i, "");
		/*
		 * This should fail -- the easy variant can't change the low
		 * water mark.
		 */
		ptr = cache_bin_alloc_easy(&bin, &success);
		expect_ptr_null(ptr, "");
		expect_false(success, "");
		expect_true(cache_bin_low_water_get(&bin, &info)
		    == ncached_max - i, "");
		expect_true(cache_bin_ncached_get(&bin, &info)
		    == ncached_max - i, "");

		/* This should succeed, though. */
		ptr = cache_bin_alloc(&bin, &success);
		expect_true(success, "");
		expect_ptr_eq(ptr, &ptrs[ncached_max - i - 1],
		    "Alloc should pop in stack order");
		expect_true(cache_bin_low_water_get(&bin, &info)
		    == ncached_max - i - 1, "");
		expect_true(cache_bin_ncached_get(&bin, &info)
		    == ncached_max - i - 1, "");
	}
	/* Now we're empty -- all alloc attempts should fail. */
	expect_true(cache_bin_ncached_get(&bin, &info) == 0, "");
	ptr = cache_bin_alloc_easy(&bin, &success);
	expect_ptr_null(ptr, "");
	expect_false(success, "");
	ptr = cache_bin_alloc(&bin, &success);
	expect_ptr_null(ptr, "");
	expect_false(success, "");

	for (cache_bin_sz_t i = 0; i < ncached_max / 2; i++) {
		cache_bin_dalloc_easy(&bin, &ptrs[i]);
	}
	cache_bin_low_water_set(&bin);

	for (cache_bin_sz_t i = ncached_max / 2; i < ncached_max; i++) {
		cache_bin_dalloc_easy(&bin, &ptrs[i]);
	}
	expect_true(cache_bin_ncached_get(&bin, &info) == ncached_max, "");
	for (cache_bin_sz_t i = ncached_max - 1; i >= ncached_max / 2; i--) {
		/*
		 * Size is bigger than low water -- the reduced version should
		 * succeed.
		 */
		ptr = cache_bin_alloc_easy(&bin, &success);
		expect_true(success, "");
		expect_ptr_eq(ptr, &ptrs[i], "");
	}
	/* But now, we've hit low-water. */
	ptr = cache_bin_alloc_easy(&bin, &success);
	expect_false(success, "");
	expect_ptr_null(ptr, "");

	/* We're going to test filling -- we must be empty to start. */
	while (cache_bin_ncached_get(&bin, &info)) {
		cache_bin_alloc(&bin, &success);
		expect_true(success, "");
	}

	/* Test fill. */
	/* Try to fill all, succeed fully. */
	do_fill_test(&bin, &info, ptrs, ncached_max, ncached_max, ncached_max);
	/* Try to fill all, succeed partially. */
	do_fill_test(&bin, &info, ptrs, ncached_max, ncached_max,
	    ncached_max / 2);
	/* Try to fill all, fail completely. */
	do_fill_test(&bin, &info, ptrs, ncached_max, ncached_max, 0);

	/* Try to fill some, succeed fully. */
	do_fill_test(&bin, &info, ptrs, ncached_max, ncached_max / 2,
	    ncached_max / 2);
	/* Try to fill some, succeed partially. */
	do_fill_test(&bin, &info, ptrs, ncached_max, ncached_max / 2,
	    ncached_max / 2);
	/* Try to fill some, fail completely. */
	do_fill_test(&bin, &info, ptrs, ncached_max, ncached_max / 2, 0);

	do_flush_test(&bin, &info, ptrs, ncached_max, ncached_max);
	do_flush_test(&bin, &info, ptrs, ncached_max, ncached_max / 2);
	do_flush_test(&bin, &info, ptrs, ncached_max, 0);
	do_flush_test(&bin, &info, ptrs, ncached_max / 2, ncached_max / 2);
	do_flush_test(&bin, &info, ptrs, ncached_max / 2, ncached_max / 4);
	do_flush_test(&bin, &info, ptrs, ncached_max / 2, 0);

	free(to_free);
	free(ptrs);
}
TEST_END

int
main(void) {
	return test(test_cache_bin);
}
