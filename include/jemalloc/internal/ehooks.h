#ifndef JEMALLOC_INTERNAL_EHOOKS_H
#define JEMALLOC_INTERNAL_EHOOKS_H

/*
 * These are exported because they get called by the inline functions below, but
 * they shouldn't really be called outside of the ehooks module.
 */
void *ehooks_alloc_default_impl(tsdn_t *tsdn, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec,
    unsigned arena_ind);
bool ehooks_dalloc_default_impl(void *addr, size_t size);
void ehooks_destroy_default_impl(void *addr, size_t size);
bool ehooks_commit_default_impl(void *addr, size_t size);
bool ehooks_decommit_default_impl(void *addr, size_t size);
bool ehooks_purge_lazy_default_impl(void *addr, size_t size);
bool ehooks_purge_forced_default_impl(void *addr, size_t size);
bool ehooks_split_default_impl();
bool ehooks_merge_default_impl(void *addr_a, void *addr_b);
extern bool opt_retain;

static inline void ehook_pre_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_pre_reentrancy_raw(tsd);
}
static inline void ehook_post_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_post_reentrancy_raw(tsd);
}

/*
 * This is the internal extent hook representation.  It abstracts over the
 * various forms of the interface we may be experimenting with at any given
 * time.  For the time being, we have only the hooks type exposed publicly in
 * 5.0, but that will change.
 */

typedef struct ehooks_s ehooks_t;
struct ehooks_s {
	/*
	 * The extent hooks API nominally takes and returns a user pointer,
	 * which is passed to the hooks when executed.  This can be used to
	 * squirrel away data adjacent to the hooks.  To ensure backwards
	 * compatability in this case, we track the pointer the hooks were
	 * originally created with, and pass it to the user hooks.
	 *
	 * This is NULL in case the extent hooks are default.
	 */
	extent_hooks_t *user_ptr;
	/* The hooks themselves. */
	extent_hooks_t hooks;
	/*
	 * Represents a dss_prec_t, but atomically.  This is only used by the
	 * default extent hooks.
	 */
	atomic_u_t dss_prec;
};

extern const extent_hooks_t extent_hooks_default;

static inline void ehooks_init(ehooks_t *ehooks, extent_hooks_t *extent_hooks) {
	ehooks->user_ptr = extent_hooks;
	memcpy(&ehooks->hooks, extent_hooks, sizeof(extent_hooks_t));
	atomic_store_u(&ehooks->dss_prec, (unsigned)extent_dss_prec_get(),
	    ATOMIC_RELAXED);
}

static inline extent_hooks_t *
ehooks_user_ptr_get(ehooks_t *ehooks) {
	return ehooks->user_ptr;
}

static inline bool
ehooks_are_default(ehooks_t *ehooks) {
	return ehooks->user_ptr == &extent_hooks_default;
}

/* See the comment at the top of ehooks.c */
#define ehook_is_default(ehooks, name) (((uintptr_t)ehooks->hooks.name) == 1)

/*
 * In some cases, we have to commit resources before attempting to split, merge,
 * or dalloc an extent.  This is wasteful if that attempt is doomed (say,
 * because the hook isn't set.  This will return true in such a case.
 */
static inline bool
ehooks_split_will_fail(ehooks_t *ehooks) {
	return !ehooks_are_default(ehooks) && ehooks->hooks.split == NULL;
}
static inline bool
ehooks_merge_will_fail(ehooks_t *ehooks) {
	return !ehooks_are_default(ehooks) && ehooks->hooks.merge == NULL;
}

static inline bool
ehooks_dalloc_will_fail(ehooks_t *ehooks) {
	if (ehooks_are_default(ehooks)) {
		/* With retain enabled, the default dalloc always fails. */
		return opt_retain;
	}
	return ehooks->hooks.dalloc == NULL;
}

/*
 * In some cases, we use VM trickery to zero out pages.  We behave
 * conservatively, though, to try to avoid breaking up hugepages or calling our
 * own functions on user-provided memory.
 */
static inline void
ehooks_zero(ehooks_t *ehooks, void *addr, size_t size) {
	if (ehooks_are_default(ehooks) && opt_thp != thp_mode_always) {
		bool err = pages_purge_forced(addr, size);
		if (!err) {
			return;
		}
	}
	memset(addr, 0, size);
}

static inline void *
ehooks_alloc(tsdn_t *tsdn, ehooks_t *ehooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	assert(size != 0);
	assert(alignment != 0);

	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, alloc)) {
		return ehooks_alloc_default_impl(tsdn, new_addr, size,
		    alignment, zero, commit,
		    (dss_prec_t)atomic_load_u(&ehooks->dss_prec,
			ATOMIC_RELAXED),
		    arena_ind);

	} else {
		ehook_pre_reentrancy(tsdn);
		void *ptr = ehooks->hooks.alloc(ehooks->user_ptr, new_addr,
		    size, alignment, zero, commit, arena_ind);
		ehook_post_reentrancy(tsdn);
		return ptr;
	}
}

static inline bool
ehooks_dalloc(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, dalloc)) {
		return ehooks_dalloc_default_impl(addr, size);
	} else if (ehooks->hooks.dalloc == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err =  ehooks->hooks.dalloc(ehooks->user_ptr, addr, size,
		    committed, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

static inline void
ehooks_destroy(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, destroy)) {
		ehooks_destroy_default_impl(addr, size);
	} else if (ehooks->hooks.destroy == NULL) {
		return;
	} else {
		ehook_pre_reentrancy(tsdn);
		ehooks->hooks.destroy(ehooks->user_ptr, addr, size, committed,
		    arena_ind);
		ehook_post_reentrancy(tsdn);
	}
}

static inline bool
ehooks_commit(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, commit)) {
		return ehooks_commit_default_impl(
		    (void *)((uintptr_t)addr + (uintptr_t)offset),
		    length);
	} else if (ehooks->hooks.commit == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err = ehooks->hooks.commit(ehooks->user_ptr, addr, size,
		    offset, length, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_decommit(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, decommit)) {
		return ehooks_decommit_default_impl(
		    (void *)((uintptr_t)addr + (uintptr_t)offset),
		    length);
	} else if (ehooks->hooks.decommit == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err = ehooks->hooks.decommit(ehooks->user_ptr, addr, size,
		    offset, length, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_purge_lazy(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	if (ehooks_are_default(ehooks)
	    || ehook_is_default(ehooks, purge_lazy)) {
		return ehooks_purge_lazy_default_impl(
		    (void *)((uintptr_t)addr + (uintptr_t)offset),
		    length);
	} else if (ehooks->hooks.purge_lazy == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err = ehooks->hooks.purge_lazy(ehooks->user_ptr, addr,
		    size, offset, length, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_purge_forced(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	if (ehooks_are_default(ehooks)
	    || ehook_is_default(ehooks, purge_lazy)) {
		return ehooks_purge_forced_default_impl(
		    (void *)((uintptr_t)addr + (uintptr_t)offset),
		    length);
	} else if (ehooks->hooks.purge_forced == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err = ehooks->hooks.purge_forced(ehooks->user_ptr, addr,
		    size, offset, length, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_split(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, split)) {
		return ehooks_split_default_impl();
	} else if (ehooks->hooks.split == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err = ehooks->hooks.split(ehooks->user_ptr, addr, size,
		    size_a, size_b, committed, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_merge(tsdn_t *tsdn, ehooks_t *ehooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	if (ehooks_are_default(ehooks) || ehook_is_default(ehooks, merge)) {
		return ehooks_merge_default_impl(addr_a, addr_b);
	} else if (ehooks->hooks.merge == NULL) {
		return true;
	} else {
		ehook_pre_reentrancy(tsdn);
		bool err = ehooks->hooks.merge(ehooks->user_ptr, addr_a,
		    size_a, addr_b, size_b, committed, arena_ind);
		ehook_post_reentrancy(tsdn);
		return err;
	}
}

#endif /* JEMALLOC_INTERNAL_EHOOKS_H */
