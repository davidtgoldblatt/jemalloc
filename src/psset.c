#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/psset.h"

#include "jemalloc/internal/flat_bitmap.h"

static const bitmap_info_t psset_bitmap_info =
    BITMAP_INFO_INITIALIZER(PSSET_NPSIZES);

void
psset_init(psset_t *psset) {
	for (unsigned i = 0; i < PSSET_NPSIZES; i++) {
		edata_heap_new(&psset->pageslabs[i]);
	}
	bitmap_init(psset->bitmap, &psset_bitmap_info, /* fill */ true);
}

/*
 * Similar to PAC's extent_recycle_extract.  Out of all the pageslabs in the
 * set, picks one that can satisfy the allocation and remove it from the set.
 */
static edata_t *
psset_recycle_extract(psset_t *psset, size_t size) {
	pszind_t ret_ind;
	edata_t *ret = NULL;
	pszind_t pind = sz_psz2ind(sz_psz_quantize_ceil(size));
	for (pszind_t i = (pszind_t)bitmap_ffu(psset->bitmap,
	    &psset_bitmap_info, (size_t)pind);
	    i < PSSET_NPSIZES;
	    i = (pszind_t)bitmap_ffu(psset->bitmap, &psset_bitmap_info,
		(size_t)i + 1)) {
		assert(!edata_heap_empty(&psset->pageslabs[i]));
		edata_t *ps = edata_heap_first(&psset->pageslabs[i]);
		if (ret == NULL || edata_snad_comp(ps, ret) < 0) {
			ret = ps;
			ret_ind = i;
		}
	}
	if (ret == NULL) {
		return NULL;
	}
	edata_heap_remove(&psset->pageslabs[ret_ind], ret);
	if (edata_heap_empty(&psset->pageslabs[ret_ind])) {
		bitmap_set(psset->bitmap, &psset_bitmap_info, ret_ind);
	}
	return ret;
}

static void
psset_insert(psset_t *psset, edata_t *ps, size_t largest_range) {
	pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
	    largest_range << LG_PAGE));

	assert(pind < PSSET_NPSIZES);

	if (edata_heap_empty(&psset->pageslabs[pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info, (size_t)pind);
	}
	edata_heap_insert(&psset->pageslabs[pind], ps);
}

/*
 * Given a pageslab ps and an edata to allocate size bytes from, initializes the
 * edata with a range in the pageslab, and puts ps back in the set.
 */
static void
psset_ps_alloc_insert(psset_t *psset, edata_t *ps, edata_t *r_edata,
    size_t size) {
	size_t start = 0;
	/*
	 * These are dead stores, but the compiler will issue warnings on them
	 * since it can't tell statically that found is always true below.
	 */
	size_t begin = 0;
	size_t len = 0;

	fb_group_t *ps_fb = edata_slab_data_get(ps)->bitmap;

	size_t npages = size >> LG_PAGE;
	size_t ps_npages = edata_size_get(ps) >> LG_PAGE;

	size_t largest_unchosen_range = 0;
	while (true) {
		bool found = fb_urange_iter(ps_fb, ps_npages, start, &begin,
		    &len);
		/*
		 * A precondition to this function is that ps must be able to
		 * serve the allocation.
		 */
		assert(found);
		if (len >= npages) {
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	uintptr_t addr = (uintptr_t)edata_base_get(ps) + begin * PAGE;
	edata_init(r_edata, edata_arena_ind_get(r_edata), (void *)addr, size,
	    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_active,
	    /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
	    EXTENT_NOT_HEAD);
	edata_ps_set(r_edata, ps);
	fb_set_range(ps_fb, ps_npages, begin, npages);
	/*
	 * OK, we've got to put the pageslab back.  First we have to figure out
	 * where, though; we've only checked run sizes before the pageslab we
	 * picked.  We also need to look for ones after the one we picked.  Note
	 * that we want begin + npages as the start position, not begin + len;
	 * we might not have used the whole range.
	 */
	start = begin + npages;
	while (start < ps_npages) {
		bool found = fb_urange_iter(ps_fb, ps_npages, start, &begin,
		    &len);
		if (!found) {
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	edata_longest_free_range_set(ps, (uint32_t)largest_unchosen_range);
	if (largest_unchosen_range != 0) {
		psset_insert(psset, ps, largest_unchosen_range);
	}
}

bool
psset_alloc_reuse(psset_t *psset, edata_t *r_edata, size_t size) {
	edata_t *ps = psset_recycle_extract(psset, size);
	if (ps == NULL) {
		return true;
	}
	psset_ps_alloc_insert(psset, ps, r_edata, size);
	return false;
}

void
psset_alloc_new(psset_t *psset, edata_t *ps, edata_t *r_edata, size_t size) {
	fb_group_t *ps_fb = edata_slab_data_get(ps)->bitmap;
	size_t ps_npages = edata_size_get(ps) >> LG_PAGE;
	assert(fb_empty(ps_fb, ps_npages));

	assert(ps_npages >= (size >> LG_PAGE));
	psset_ps_alloc_insert(psset, ps, r_edata, size);
}

edata_t *
psset_dalloc(psset_t *psset, edata_t *edata) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_ps_get(edata) != NULL);

	edata_t *ps = edata_ps_get(edata);
	fb_group_t *ps_fb = edata_slab_data_get(ps)->bitmap;
	size_t ps_old_longest_free_range = edata_longest_free_range_get(ps);

	size_t ps_npages = edata_size_get(ps) >> LG_PAGE;
	size_t begin =
	    ((uintptr_t)edata_base_get(edata) - (uintptr_t)edata_base_get(ps))
	    >> LG_PAGE;
	size_t len = edata_size_get(edata) >> LG_PAGE;
	fb_unset_range(ps_fb, ps_npages, begin, len);

	/* We might have just created a new, larger range. */
	size_t new_begin = (size_t)(fb_fls(ps_fb, ps_npages, begin) + 1);
	size_t new_end = fb_ffs(ps_fb, ps_npages, begin + len - 1);
	size_t new_range_len = new_end - new_begin;
	/*
	 * If the new free range is no longer than the previous longest one,
	 * then the pageslab is non-empty and doesn't need to change bins.
	 * We're done, and don't need to return a pageslab to evict.
	 */
	if (new_range_len <= ps_old_longest_free_range) {
		return NULL;
	}
	/*
	 * Otherwise, it might need to get evicted from the set, or change its
	 * bin.
	 */
	edata_longest_free_range_set(ps, (uint32_t)new_range_len);
	/*
	 * If it was previously non-full, then it's in some (possibly now
	 * incorrect) bin already; remove it.
	 */
	if (ps_old_longest_free_range > 0) {
		pszind_t old_pind = sz_psz2ind(sz_psz_quantize_floor(
		    ps_old_longest_free_range<< LG_PAGE));
		edata_heap_remove(&psset->pageslabs[old_pind], ps);
		if (edata_heap_empty(&psset->pageslabs[old_pind])) {
			bitmap_set(psset->bitmap, &psset_bitmap_info,
			    (size_t)old_pind);
		}
	}
	/* If the pageslab is empty, it gets evicted from the set. */
	if (new_range_len == ps_npages) {
		return ps;
	}
	/* Otherwise, it gets reinserted. */
	pszind_t new_pind = sz_psz2ind(sz_psz_quantize_floor(
	    new_range_len << LG_PAGE));
	if (edata_heap_empty(&psset->pageslabs[new_pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info,
		    (size_t)new_pind);
	}
	edata_heap_insert(&psset->pageslabs[new_pind], ps);
	return NULL;
}
