#define JEMALLOC_STATS_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emitter.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_prof.h"

const char *global_mutex_names[mutex_prof_num_global_mutexes] = {
#define OP(mtx) #mtx,
	MUTEX_PROF_GLOBAL_MUTEXES
#undef OP
};

const char *arena_mutex_names[mutex_prof_num_arena_mutexes] = {
#define OP(mtx) #mtx,
	MUTEX_PROF_ARENA_MUTEXES
#undef OP
};

#define CTL_GET(n, v, t) do {						\
	size_t sz = sizeof(t);						\
	xmallctl(n, (void *)v, &sz, NULL, 0);				\
} while (0)

#define CTL_M2_GET(n, i, v, t) do {					\
	size_t mib[CTL_MAX_DEPTH];					\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	xmallctlbymib(mib, miblen, (void *)v, &sz, NULL, 0);		\
} while (0)

#define CTL_M2_M4_GET(n, i, j, v, t) do {				\
	size_t mib[CTL_MAX_DEPTH];					\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	mib[4] = (j);							\
	xmallctlbymib(mib, miblen, (void *)v, &sz, NULL, 0);		\
} while (0)

/******************************************************************************/
/* Data. */

bool opt_stats_print = false;
char opt_stats_print_opts[stats_print_tot_num_options+1] = "";

/******************************************************************************/

/* Calculate x.yyy and output a string (takes a fixed sized char array). */
static bool
get_rate_str(uint64_t dividend, uint64_t divisor, char str[6]) {
	if (divisor == 0 || dividend > divisor) {
		/* The rate is not supposed to be greater than 1. */
		return true;
	}
	if (dividend > 0) {
		assert(UINT64_MAX / dividend >= 1000);
	}

	unsigned n = (unsigned)((dividend * 1000) / divisor);
	if (n < 10) {
		malloc_snprintf(str, 6, "0.00%u", n);
	} else if (n < 100) {
		malloc_snprintf(str, 6, "0.0%u", n);
	} else if (n < 1000) {
		malloc_snprintf(str, 6, "0.%u", n);
	} else {
		malloc_snprintf(str, 6, "1");
	}

	return false;
}

#define MUTEX_CTL_STR_MAX_LENGTH 128
static void
gen_mutex_ctl_str(char *str, size_t buf_len, const char *prefix,
    const char *mutex, const char *counter) {
	malloc_snprintf(str, buf_len, "stats.%s.%s.%s", prefix, mutex, counter);
}

static void
read_arena_bin_mutex_stats(unsigned arena_ind, unsigned bin_ind,
    uint64_t results[mutex_prof_num_counters]) {
	char cmd[MUTEX_CTL_STR_MAX_LENGTH];
#define OP(c, t)							\
    gen_mutex_ctl_str(cmd, MUTEX_CTL_STR_MAX_LENGTH,			\
        "arenas.0.bins.0","mutex", #c);					\
    CTL_M2_M4_GET(cmd, arena_ind, bin_ind,				\
        (t *)&results[mutex_counter_##c], t);
MUTEX_PROF_COUNTERS
#undef OP
}

static void
mutex_stats_output_json(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *name, uint64_t stats[mutex_prof_num_counters],
    const char *json_indent, bool last) {
	malloc_cprintf(write_cb, cbopaque, "%s\"%s\": {\n", json_indent, name);

	mutex_prof_counter_ind_t k = 0;
	char *fmt_str[2] = {"%s\t\"%s\": %"FMTu32"%s\n",
	    "%s\t\"%s\": %"FMTu64"%s\n"};
#define OP(c, t)							\
	malloc_cprintf(write_cb, cbopaque,				\
	    fmt_str[sizeof(t) / sizeof(uint32_t) - 1], 			\
	    json_indent, #c, (t)stats[mutex_counter_##c],		\
	    (++k == mutex_prof_num_counters) ? "" : ",");
MUTEX_PROF_COUNTERS
#undef OP
	malloc_cprintf(write_cb, cbopaque, "%s}%s\n", json_indent,
	    last ? "" : ",");
}

static void
stats_arena_bins_print(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, bool large, bool mutex, unsigned i) {
	size_t page;
	bool in_gap, in_gap_prev;
	unsigned nbins, j;

	CTL_GET("arenas.page", &page, size_t);

	CTL_GET("arenas.nbins", &nbins, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"bins\": [\n");
	} else {
		char *mutex_counters = "   n_lock_ops    n_waiting"
		    "   n_spin_acq n_owner_switch  total_wait_ns"
		    "  max_wait_ns max_n_thds\n";
		malloc_cprintf(write_cb, cbopaque,
		    "bins:           size ind    allocated      nmalloc"
		    "      ndalloc    nrequests      curregs     curslabs regs"
		    " pgs  util       nfills     nflushes     newslabs"
		    "      reslabs%s", mutex ? mutex_counters : "\n");
	}
	for (j = 0, in_gap = false; j < nbins; j++) {
		uint64_t nslabs;
		size_t reg_size, slab_size, curregs;
		size_t curslabs;
		uint32_t nregs;
		uint64_t nmalloc, ndalloc, nrequests, nfills, nflushes;
		uint64_t nreslabs;

		CTL_M2_M4_GET("stats.arenas.0.bins.0.nslabs", i, j, &nslabs,
		    uint64_t);
		in_gap_prev = in_gap;
		in_gap = (nslabs == 0);

		if (!json && in_gap_prev && !in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}

		CTL_M2_GET("arenas.bin.0.size", j, &reg_size, size_t);
		CTL_M2_GET("arenas.bin.0.nregs", j, &nregs, uint32_t);
		CTL_M2_GET("arenas.bin.0.slab_size", j, &slab_size, size_t);

		CTL_M2_M4_GET("stats.arenas.0.bins.0.nmalloc", i, j, &nmalloc,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.ndalloc", i, j, &ndalloc,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.curregs", i, j, &curregs,
		    size_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.nrequests", i, j,
		    &nrequests, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.nfills", i, j, &nfills,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.nflushes", i, j, &nflushes,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.nreslabs", i, j, &nreslabs,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.curslabs", i, j, &curslabs,
		    size_t);

		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t{\n"
			    "\t\t\t\t\t\t\"nmalloc\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"ndalloc\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"curregs\": %zu,\n"
			    "\t\t\t\t\t\t\"nrequests\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"nfills\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"nflushes\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"nreslabs\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"curslabs\": %zu%s\n",
			    nmalloc, ndalloc, curregs, nrequests, nfills,
			    nflushes, nreslabs, curslabs, mutex ? "," : "");
			if (mutex) {
				uint64_t mutex_stats[mutex_prof_num_counters];
				read_arena_bin_mutex_stats(i, j, mutex_stats);
				mutex_stats_output_json(write_cb, cbopaque,
				    "mutex", mutex_stats, "\t\t\t\t\t\t", true);
			}
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t}%s\n",
			    (j + 1 < nbins) ? "," : "");
		} else if (!in_gap) {
			size_t availregs = nregs * curslabs;
			char util[6];
			if (get_rate_str((uint64_t)curregs, (uint64_t)availregs,
			    util)) {
				if (availregs == 0) {
					malloc_snprintf(util, sizeof(util),
					    "1");
				} else if (curregs > availregs) {
					/*
					 * Race detected: the counters were read
					 * in separate mallctl calls and
					 * concurrent operations happened in
					 * between. In this case no meaningful
					 * utilization can be computed.
					 */
					malloc_snprintf(util, sizeof(util),
					    " race");
				} else {
					not_reached();
				}
			}
			uint64_t mutex_stats[mutex_prof_num_counters];
			if (mutex) {
				read_arena_bin_mutex_stats(i, j, mutex_stats);
			}

			malloc_cprintf(write_cb, cbopaque, "%20zu %3u %12zu %12"
			    FMTu64" %12"FMTu64" %12"FMTu64" %12zu %12zu %4u"
			    " %3zu %-5s %12"FMTu64" %12"FMTu64" %12"FMTu64
			    " %12"FMTu64, reg_size, j, curregs * reg_size,
			    nmalloc, ndalloc, nrequests, curregs, curslabs,
			    nregs, slab_size / page, util, nfills, nflushes,
			    nslabs, nreslabs);

			if (mutex) {
				malloc_cprintf(write_cb, cbopaque,
				    " %12"FMTu64" %12"FMTu64" %12"FMTu64
				    " %14"FMTu64" %14"FMTu64" %12"FMTu64
				    " %10"FMTu64"\n",
				    mutex_stats[mutex_counter_num_ops],
				    mutex_stats[mutex_counter_num_wait],
				    mutex_stats[mutex_counter_num_spin_acq],
				    mutex_stats[mutex_counter_num_owner_switch],
				    mutex_stats[mutex_counter_total_wait_time],
				    mutex_stats[mutex_counter_max_wait_time],
				    mutex_stats[mutex_counter_max_num_thds]);
			} else {
				malloc_cprintf(write_cb, cbopaque, "\n");
			}
		}
	}
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t]%s\n", large ? "," : "");
	} else {
		if (in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}
	}
}

static void
stats_arena_lextents_print(void (*write_cb)(void *, const char *),
    void *cbopaque, bool json, unsigned i) {
	unsigned nbins, nlextents, j;
	bool in_gap, in_gap_prev;

	CTL_GET("arenas.nbins", &nbins, unsigned);
	CTL_GET("arenas.nlextents", &nlextents, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"lextents\": [\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "large:          size ind    allocated      nmalloc"
		    "      ndalloc    nrequests  curlextents\n");
	}
	for (j = 0, in_gap = false; j < nlextents; j++) {
		uint64_t nmalloc, ndalloc, nrequests;
		size_t lextent_size, curlextents;

		CTL_M2_M4_GET("stats.arenas.0.lextents.0.nmalloc", i, j,
		    &nmalloc, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.lextents.0.ndalloc", i, j,
		    &ndalloc, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.lextents.0.nrequests", i, j,
		    &nrequests, uint64_t);
		in_gap_prev = in_gap;
		in_gap = (nrequests == 0);

		if (!json && in_gap_prev && !in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}

		CTL_M2_GET("arenas.lextent.0.size", j, &lextent_size, size_t);
		CTL_M2_M4_GET("stats.arenas.0.lextents.0.curlextents", i, j,
		    &curlextents, size_t);
		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t{\n"
			    "\t\t\t\t\t\t\"curlextents\": %zu\n"
			    "\t\t\t\t\t}%s\n",
			    curlextents,
			    (j + 1 < nlextents) ? "," : "");
		} else if (!in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "%20zu %3u %12zu %12"FMTu64" %12"FMTu64
			    " %12"FMTu64" %12zu\n",
			    lextent_size, nbins + j,
			    curlextents * lextent_size, nmalloc, ndalloc,
			    nrequests, curlextents);
		}
	}
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t]\n");
	} else {
		if (in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}
	}
}

static void
read_arena_mutex_stats(unsigned arena_ind,
    uint64_t results[mutex_prof_num_arena_mutexes][mutex_prof_num_counters]) {
	char cmd[MUTEX_CTL_STR_MAX_LENGTH];

	mutex_prof_arena_ind_t i;
	for (i = 0; i < mutex_prof_num_arena_mutexes; i++) {
#define OP(c, t)							\
		gen_mutex_ctl_str(cmd, MUTEX_CTL_STR_MAX_LENGTH,	\
		    "arenas.0.mutexes",	arena_mutex_names[i], #c);	\
		CTL_M2_GET(cmd, arena_ind,				\
		    (t *)&results[i][mutex_counter_##c], t);
MUTEX_PROF_COUNTERS
#undef OP
	}
}

static void
mutex_stats_output(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *name, uint64_t stats[mutex_prof_num_counters],
    bool first_mutex) {
	if (first_mutex) {
		/* Print title. */
		malloc_cprintf(write_cb, cbopaque,
		    "                           n_lock_ops       n_waiting"
		    "      n_spin_acq  n_owner_switch   total_wait_ns"
		    "     max_wait_ns  max_n_thds\n");
	}

	malloc_cprintf(write_cb, cbopaque, "%s", name);
	malloc_cprintf(write_cb, cbopaque, ":%*c",
	    (int)(20 - strlen(name)), ' ');

	char *fmt_str[2] = {"%12"FMTu32, "%16"FMTu64};
#define OP(c, t)							\
	malloc_cprintf(write_cb, cbopaque,				\
	    fmt_str[sizeof(t) / sizeof(uint32_t) - 1],			\
	    (t)stats[mutex_counter_##c]);
MUTEX_PROF_COUNTERS
#undef OP
	malloc_cprintf(write_cb, cbopaque, "\n");
}

static void
stats_arena_mutexes_print(void (*write_cb)(void *, const char *),
    void *cbopaque, bool json, bool json_end, unsigned arena_ind) {
	uint64_t mutex_stats[mutex_prof_num_arena_mutexes][mutex_prof_num_counters];
	read_arena_mutex_stats(arena_ind, mutex_stats);

	/* Output mutex stats. */
	if (json) {
		malloc_cprintf(write_cb, cbopaque, "\t\t\t\t\"mutexes\": {\n");
		mutex_prof_arena_ind_t i, last_mutex;
		last_mutex = mutex_prof_num_arena_mutexes - 1;
		for (i = 0; i < mutex_prof_num_arena_mutexes; i++) {
			mutex_stats_output_json(write_cb, cbopaque,
			    arena_mutex_names[i], mutex_stats[i],
			    "\t\t\t\t\t", (i == last_mutex));
		}
		malloc_cprintf(write_cb, cbopaque, "\t\t\t\t}%s\n",
		    json_end ? "" : ",");
	} else {
		mutex_prof_arena_ind_t i;
		for (i = 0; i < mutex_prof_num_arena_mutexes; i++) {
			mutex_stats_output(write_cb, cbopaque,
			    arena_mutex_names[i], mutex_stats[i], i == 0);
		}
	}
}

static void
stats_arena_print(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, unsigned i, bool bins, bool large, bool mutex) {
	unsigned nthreads;
	const char *dss;
	ssize_t dirty_decay_ms, muzzy_decay_ms;
	size_t page, pactive, pdirty, pmuzzy, mapped, retained;
	size_t base, internal, resident, metadata_thp;
	uint64_t dirty_npurge, dirty_nmadvise, dirty_purged;
	uint64_t muzzy_npurge, muzzy_nmadvise, muzzy_purged;
	size_t small_allocated;
	uint64_t small_nmalloc, small_ndalloc, small_nrequests;
	size_t large_allocated;
	uint64_t large_nmalloc, large_ndalloc, large_nrequests;
	size_t tcache_bytes;
	uint64_t uptime;

	CTL_GET("arenas.page", &page, size_t);

	CTL_M2_GET("stats.arenas.0.nthreads", i, &nthreads, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"nthreads\": %u,\n", nthreads);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "assigned threads: %u\n", nthreads);
	}

	CTL_M2_GET("stats.arenas.0.uptime", i, &uptime, uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"uptime_ns\": %"FMTu64",\n", uptime);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "uptime: %"FMTu64"\n", uptime);
	}

	CTL_M2_GET("stats.arenas.0.dss", i, &dss, const char *);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dss\": \"%s\",\n", dss);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "dss allocation precedence: %s\n", dss);
	}

	CTL_M2_GET("stats.arenas.0.dirty_decay_ms", i, &dirty_decay_ms,
	    ssize_t);
	CTL_M2_GET("stats.arenas.0.muzzy_decay_ms", i, &muzzy_decay_ms,
	    ssize_t);
	CTL_M2_GET("stats.arenas.0.pactive", i, &pactive, size_t);
	CTL_M2_GET("stats.arenas.0.pdirty", i, &pdirty, size_t);
	CTL_M2_GET("stats.arenas.0.pmuzzy", i, &pmuzzy, size_t);
	CTL_M2_GET("stats.arenas.0.dirty_npurge", i, &dirty_npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_nmadvise", i, &dirty_nmadvise,
	    uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_purged", i, &dirty_purged, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_npurge", i, &muzzy_npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_nmadvise", i, &muzzy_nmadvise,
	    uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_purged", i, &muzzy_purged, uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_decay_ms\": %zd,\n", dirty_decay_ms);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_decay_ms\": %zd,\n", muzzy_decay_ms);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"pactive\": %zu,\n", pactive);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"pdirty\": %zu,\n", pdirty);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"pmuzzy\": %zu,\n", pmuzzy);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_npurge\": %"FMTu64",\n", dirty_npurge);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_nmadvise\": %"FMTu64",\n", dirty_nmadvise);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_purged\": %"FMTu64",\n", dirty_purged);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_npurge\": %"FMTu64",\n", muzzy_npurge);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_nmadvise\": %"FMTu64",\n", muzzy_nmadvise);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_purged\": %"FMTu64",\n", muzzy_purged);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "decaying:  time       npages       sweeps     madvises"
		    "       purged\n");
		if (dirty_decay_ms >= 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "   dirty: %5zd %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", dirty_decay_ms, pdirty, dirty_npurge,
			    dirty_nmadvise, dirty_purged);
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "   dirty:   N/A %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", pdirty, dirty_npurge, dirty_nmadvise,
			    dirty_purged);
		}
		if (muzzy_decay_ms >= 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "   muzzy: %5zd %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", muzzy_decay_ms, pmuzzy, muzzy_npurge,
			    muzzy_nmadvise, muzzy_purged);
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "   muzzy:   N/A %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", pmuzzy, muzzy_npurge, muzzy_nmadvise,
			    muzzy_purged);
		}
	}

	CTL_M2_GET("stats.arenas.0.small.allocated", i, &small_allocated,
	    size_t);
	CTL_M2_GET("stats.arenas.0.small.nmalloc", i, &small_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.ndalloc", i, &small_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.nrequests", i, &small_nrequests,
	    uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"small\": {\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"allocated\": %zu,\n", small_allocated);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nmalloc\": %"FMTu64",\n", small_nmalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"ndalloc\": %"FMTu64",\n", small_ndalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nrequests\": %"FMTu64"\n", small_nrequests);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t},\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "                            allocated      nmalloc"
		    "      ndalloc    nrequests\n");
		malloc_cprintf(write_cb, cbopaque,
		    "small:                   %12zu %12"FMTu64" %12"FMTu64
		    " %12"FMTu64"\n",
		    small_allocated, small_nmalloc, small_ndalloc,
		    small_nrequests);
	}

	CTL_M2_GET("stats.arenas.0.large.allocated", i, &large_allocated,
	    size_t);
	CTL_M2_GET("stats.arenas.0.large.nmalloc", i, &large_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.ndalloc", i, &large_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.nrequests", i, &large_nrequests,
	    uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"large\": {\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"allocated\": %zu,\n", large_allocated);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nmalloc\": %"FMTu64",\n", large_nmalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"ndalloc\": %"FMTu64",\n", large_ndalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nrequests\": %"FMTu64"\n", large_nrequests);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t},\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "large:                   %12zu %12"FMTu64" %12"FMTu64
		    " %12"FMTu64"\n",
		    large_allocated, large_nmalloc, large_ndalloc,
		    large_nrequests);
		malloc_cprintf(write_cb, cbopaque,
		    "total:                   %12zu %12"FMTu64" %12"FMTu64
		    " %12"FMTu64"\n",
		    small_allocated + large_allocated, small_nmalloc +
		    large_nmalloc, small_ndalloc + large_ndalloc,
		    small_nrequests + large_nrequests);
	}
	if (!json) {
		malloc_cprintf(write_cb, cbopaque,
		    "active:                  %12zu\n", pactive * page);
	}

	CTL_M2_GET("stats.arenas.0.mapped", i, &mapped, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"mapped\": %zu,\n", mapped);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "mapped:                  %12zu\n", mapped);
	}

	CTL_M2_GET("stats.arenas.0.retained", i, &retained, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"retained\": %zu,\n", retained);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "retained:                %12zu\n", retained);
	}

	CTL_M2_GET("stats.arenas.0.base", i, &base, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"base\": %zu,\n", base);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "base:                    %12zu\n", base);
	}

	CTL_M2_GET("stats.arenas.0.internal", i, &internal, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"internal\": %zu,\n", internal);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "internal:                %12zu\n", internal);
	}

	CTL_M2_GET("stats.arenas.0.metadata_thp", i, &metadata_thp, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"metadata_thp\": %zu,\n", metadata_thp);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "metadata_thp:            %12zu\n", metadata_thp);
	}

	CTL_M2_GET("stats.arenas.0.tcache_bytes", i, &tcache_bytes, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"tcache\": %zu,\n", tcache_bytes);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "tcache:                  %12zu\n", tcache_bytes);
	}

	CTL_M2_GET("stats.arenas.0.resident", i, &resident, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"resident\": %zu%s\n", resident,
		    (bins || large || mutex) ? "," : "");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "resident:                %12zu\n", resident);
	}

	if (mutex) {
		stats_arena_mutexes_print(write_cb, cbopaque, json,
		    !(bins || large), i);
	}
	if (bins) {
		stats_arena_bins_print(write_cb, cbopaque, json, large, mutex,
		    i);
	}
	if (large) {
		stats_arena_lextents_print(write_cb, cbopaque, json, i);
	}
}

static void
stats_general_print(emitter_t *emitter, bool more) {
	/*
	 * These should eventually be deleted; they are useful in converting
	 * from manual to emitter-based stats output, though.
	 */
	void (*write_cb)(void *, const char *) = emitter->write_cb;
	void *cbopaque = emitter->cbopaque;
	bool json = (emitter->output == emitter_output_json);

	const char *cpv;
	bool bv, bv2;
	unsigned uv;
	uint32_t u32v;
	uint64_t u64v;
	ssize_t ssv, ssv2;
	size_t sv, bsz, usz, ssz, sssz, cpsz;

	bsz = sizeof(bool);
	usz = sizeof(unsigned);
	ssz = sizeof(size_t);
	sssz = sizeof(ssize_t);
	cpsz = sizeof(const char *);

	CTL_GET("version", &cpv, const char *);
	emitter_simple_kv(emitter, "version", "Version", emitter_type_string,
	    &cpv);

	/* config. */
	emitter_vdict_begin(emitter, "config", "Build-time option settings");
#define CONFIG_WRITE_BOOL(name)						\
	do {								\
		CTL_GET("config."#name, &bv, bool);			\
		emitter_vdict_kv(emitter, #name, emitter_type_bool,	\
		    &bv);						\
	} while (0)

	CONFIG_WRITE_BOOL(cache_oblivious);
	CONFIG_WRITE_BOOL(debug);
	CONFIG_WRITE_BOOL(fill);
	CONFIG_WRITE_BOOL(lazy_lock);
	emitter_vdict_kv(emitter, "malloc_conf", emitter_type_string,
	    &config_malloc_conf);
	CONFIG_WRITE_BOOL(prof);

	CONFIG_WRITE_BOOL(prof);
	CONFIG_WRITE_BOOL(prof_libgcc);
	CONFIG_WRITE_BOOL(prof_libunwind);
	CONFIG_WRITE_BOOL(stats);
	CONFIG_WRITE_BOOL(thp);
	CONFIG_WRITE_BOOL(utrace);
	CONFIG_WRITE_BOOL(xmalloc);
#undef CONFIG_WRITE_BOOL
	emitter_vdict_end(emitter); /* Close "config" vdict. */

	/* opt. */
#define OPT_WRITE(name, var, size, emitter_type)			\
	if (je_mallctl("opt."#name, (void *)&var, &size, NULL, 0) ==	\
	    0) {							\
		emitter_vdict_kv(emitter, #name, emitter_type, &var);	\
	}

#define OPT_WRITE_MUTABLE(name, var1, var2, size, emitter_type,		\
    altname)								\
	if (je_mallctl("opt."#name, (void *)&var1, &size, NULL, 0) ==	\
	    0 && je_mallctl(#altname, (void *)&var2, &size, NULL, 0)	\
	    == 0) {							\
		emitter_vdict_kv_note(emitter, #name, emitter_type,	\
		    &var1, #altname, emitter_type, (void *)&var2);	\
	}

#define OPT_WRITE_BOOL(name) OPT_WRITE(name, bv, bsz, emitter_type_bool)
#define OPT_WRITE_BOOL_MUTABLE(name, altname)				\
	OPT_WRITE_MUTABLE(name, bv, bv2, bsz, emitter_type_bool, altname)

#define OPT_WRITE_UNSIGNED(name)					\
	OPT_WRITE(name, uv, usz, emitter_type_unsigned)

#define OPT_WRITE_SSIZE_T(name)						\
	OPT_WRITE(name, ssv, sssz, emitter_type_ssize)
#define OPT_WRITE_SSIZE_T_MUTABLE(name, altname)			\
	OPT_WRITE_MUTABLE(name, ssv, ssv2, sssz, emitter_type_ssize,	\
	    altname)

#define OPT_WRITE_CHAR_P(name)						\
	OPT_WRITE(name, cpv, cpsz, emitter_type_string)

	emitter_vdict_begin(emitter, "opt", "Run-time option settings");

	OPT_WRITE_BOOL(abort)
	OPT_WRITE_BOOL(abort_conf)
	OPT_WRITE_BOOL(retain)
	OPT_WRITE_CHAR_P(dss)
	OPT_WRITE_UNSIGNED(narenas)
	OPT_WRITE_CHAR_P(percpu_arena)
	OPT_WRITE_CHAR_P(metadata_thp)
	OPT_WRITE_BOOL_MUTABLE(background_thread, background_thread)
	OPT_WRITE_SSIZE_T_MUTABLE(dirty_decay_ms, arenas.dirty_decay_ms)
	OPT_WRITE_SSIZE_T_MUTABLE(muzzy_decay_ms, arenas.muzzy_decay_ms)
	OPT_WRITE_UNSIGNED(lg_extent_max_active_fit)
	OPT_WRITE_CHAR_P(junk)
	OPT_WRITE_BOOL(zero)
	OPT_WRITE_BOOL(utrace)
	OPT_WRITE_BOOL(xmalloc)
	OPT_WRITE_BOOL(tcache)
	OPT_WRITE_SSIZE_T(lg_tcache_max)
	OPT_WRITE_BOOL(prof)
	OPT_WRITE_CHAR_P(prof_prefix)
	OPT_WRITE_BOOL_MUTABLE(prof_active, prof.active)
	OPT_WRITE_BOOL_MUTABLE(prof_thread_active_init, prof.thread_active_init)
	OPT_WRITE_SSIZE_T_MUTABLE(lg_prof_sample, prof.lg_sample)
	OPT_WRITE_BOOL(prof_accum)
	OPT_WRITE_SSIZE_T(lg_prof_interval)
	OPT_WRITE_BOOL(prof_gdump)
	OPT_WRITE_BOOL(prof_final)
	OPT_WRITE_BOOL(prof_leak)
	OPT_WRITE_BOOL(stats_print)
	OPT_WRITE_CHAR_P(stats_print_opts)

	emitter_vdict_end(emitter);

#undef OPT_WRITE
#undef OPT_WRITE_MUTABLE
#undef OPT_WRITE_BOOL
#undef OPT_WRITE_BOOL_MUTABLE
#undef OPT_WRITE_UNSIGNED
#undef OPT_WRITE_SSIZE_T
#undef OPT_WRITE_SSIZE_T_MUTABLE
#undef OPT_WRITE_CHAR_P

	/* arenas. */
	/*
	 * The json output sticks arena info into an "arenas" dict; the table
	 * output puts them at the top-level.
	 */
	emitter_json_dict_begin(emitter, "arenas");

	CTL_GET("arenas.narenas", &uv, unsigned);
	emitter_simple_kv(emitter, "narenas", "Arenas", emitter_type_unsigned,
	    &uv);

	/* Decay settings are emitted as table notes, in the opt dict. */
	CTL_GET("arenas.dirty_decay_ms", &ssv, ssize_t);
	emitter_json_simple_kv(emitter, "dirty_decay_ms", emitter_type_ssize,
	    &ssv);

	CTL_GET("arenas.muzzy_decay_ms", &ssv, ssize_t);
	emitter_json_simple_kv(emitter, "muzzy_decay_ms", emitter_type_ssize,
	    &ssv);

	CTL_GET("arenas.quantum", &sv, size_t);
	emitter_simple_kv(emitter, "quantum", "Quantum size", emitter_type_size,
	    &sv);

	CTL_GET("arenas.page", &sv, size_t);
	emitter_simple_kv(emitter, "page", "Page size", emitter_type_size, &sv);


	if (je_mallctl("arenas.tcache_max", (void *)&sv, &ssz, NULL, 0) == 0) {
		emitter_simple_kv(emitter, "tcache_max",
		    "Maximum thread-cached size class", emitter_type_size, &sv);
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque, ",\n");

		unsigned nbins, nlextents, i;

		CTL_GET("arenas.nbins", &nbins, unsigned);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"nbins\": %u,\n", nbins);

		CTL_GET("arenas.nhbins", &uv, unsigned);
		malloc_cprintf(write_cb, cbopaque, "\t\t\t\"nhbins\": %u,\n",
		    uv);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"bin\": [\n");
		for (i = 0; i < nbins; i++) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t{\n");

			CTL_M2_GET("arenas.bin.0.size", i, &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"size\": %zu,\n", sv);

			CTL_M2_GET("arenas.bin.0.nregs", i, &u32v, uint32_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"nregs\": %"FMTu32",\n", u32v);

			CTL_M2_GET("arenas.bin.0.slab_size", i, &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"slab_size\": %zu\n", sv);

			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t}%s\n", (i + 1 < nbins) ? "," : "");
		}
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t],\n");

		CTL_GET("arenas.nlextents", &nlextents, unsigned);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"nlextents\": %u,\n", nlextents);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"lextent\": [\n");
		for (i = 0; i < nlextents; i++) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t{\n");

			CTL_M2_GET("arenas.lextent.0.size", i, &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"size\": %zu\n", sv);

			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t}%s\n", (i + 1 < nlextents) ? "," : "");
		}
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t]\n");
	}

	emitter_json_dict_end(emitter); /* Close "arenas" */

	if (json) {
		if (more || config_prof) {
			malloc_cprintf(write_cb, cbopaque, ",\n");
		} else {
			malloc_cprintf(write_cb, cbopaque, "\n");
		}
	}

	/* prof. */
	if (config_prof && json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"prof\": {\n");

		CTL_GET("prof.thread_active_init", &bv, bool);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"thread_active_init\": %s,\n", bv ? "true" :
		    "false");

		CTL_GET("prof.active", &bv, bool);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"active\": %s,\n", bv ? "true" : "false");

		CTL_GET("prof.gdump", &bv, bool);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"gdump\": %s,\n", bv ? "true" : "false");

		CTL_GET("prof.interval", &u64v, uint64_t);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"interval\": %"FMTu64",\n", u64v);

		CTL_GET("prof.lg_sample", &ssv, ssize_t);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"lg_sample\": %zd\n", ssv);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t}%s\n", more ? "," : "");
	}
}

static void
read_global_mutex_stats(
    uint64_t results[mutex_prof_num_global_mutexes][mutex_prof_num_counters]) {
	char cmd[MUTEX_CTL_STR_MAX_LENGTH];

	mutex_prof_global_ind_t i;
	for (i = 0; i < mutex_prof_num_global_mutexes; i++) {
#define OP(c, t)							\
		gen_mutex_ctl_str(cmd, MUTEX_CTL_STR_MAX_LENGTH,	\
		    "mutexes", global_mutex_names[i], #c);		\
		CTL_GET(cmd, (t *)&results[i][mutex_counter_##c], t);
MUTEX_PROF_COUNTERS
#undef OP
	}
}

static void
stats_print_helper(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, bool merged, bool destroyed, bool unmerged, bool bins,
    bool large, bool mutex) {
	size_t allocated, active, metadata, metadata_thp, resident, mapped,
	    retained;
	size_t num_background_threads;
	uint64_t background_thread_num_runs, background_thread_run_interval;

	CTL_GET("stats.allocated", &allocated, size_t);
	CTL_GET("stats.active", &active, size_t);
	CTL_GET("stats.metadata", &metadata, size_t);
	CTL_GET("stats.metadata_thp", &metadata_thp, size_t);
	CTL_GET("stats.resident", &resident, size_t);
	CTL_GET("stats.mapped", &mapped, size_t);
	CTL_GET("stats.retained", &retained, size_t);

	uint64_t mutex_stats[mutex_prof_num_global_mutexes][mutex_prof_num_counters];
	if (mutex) {
		read_global_mutex_stats(mutex_stats);
	}

	if (have_background_thread) {
		CTL_GET("stats.background_thread.num_threads",
		    &num_background_threads, size_t);
		CTL_GET("stats.background_thread.num_runs",
		    &background_thread_num_runs, uint64_t);
		CTL_GET("stats.background_thread.run_interval",
		    &background_thread_run_interval, uint64_t);
	} else {
		num_background_threads = 0;
		background_thread_num_runs = 0;
		background_thread_run_interval = 0;
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"stats\": {\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"allocated\": %zu,\n", allocated);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"active\": %zu,\n", active);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"metadata\": %zu,\n", metadata);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"metadata_thp\": %zu,\n", metadata_thp);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"resident\": %zu,\n", resident);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"mapped\": %zu,\n", mapped);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"retained\": %zu,\n", retained);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"background_thread\": {\n");
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"num_threads\": %zu,\n", num_background_threads);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"num_runs\": %"FMTu64",\n",
		    background_thread_num_runs);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"run_interval\": %"FMTu64"\n",
		    background_thread_run_interval);
		malloc_cprintf(write_cb, cbopaque, "\t\t\t}%s\n",
		    mutex ? "," : "");

		if (mutex) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\"mutexes\": {\n");
			mutex_prof_global_ind_t i;
			for (i = 0; i < mutex_prof_num_global_mutexes; i++) {
				mutex_stats_output_json(write_cb, cbopaque,
				    global_mutex_names[i], mutex_stats[i],
				    "\t\t\t\t",
				    i == mutex_prof_num_global_mutexes - 1);
			}
			malloc_cprintf(write_cb, cbopaque, "\t\t\t}\n");
		}
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t}%s\n", (merged || unmerged || destroyed) ? "," : "");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "Allocated: %zu, active: %zu, metadata: %zu (n_thp %zu),"
		    " resident: %zu, mapped: %zu, retained: %zu\n",
		    allocated, active, metadata, metadata_thp, resident, mapped,
		    retained);

		if (have_background_thread && num_background_threads > 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "Background threads: %zu, num_runs: %"FMTu64", "
			    "run_interval: %"FMTu64" ns\n",
			    num_background_threads,
			    background_thread_num_runs,
			    background_thread_run_interval);
		}
		if (mutex) {
			mutex_prof_global_ind_t i;
			for (i = 0; i < mutex_prof_num_global_mutexes; i++) {
				mutex_stats_output(write_cb, cbopaque,
				    global_mutex_names[i], mutex_stats[i],
				    i == 0);
			}
		}
	}

	if (merged || destroyed || unmerged) {
		unsigned narenas;

		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\"stats.arenas\": {\n");
		}

		CTL_GET("arenas.narenas", &narenas, unsigned);
		{
			size_t mib[3];
			size_t miblen = sizeof(mib) / sizeof(size_t);
			size_t sz;
			VARIABLE_ARRAY(bool, initialized, narenas);
			bool destroyed_initialized;
			unsigned i, j, ninitialized;

			xmallctlnametomib("arena.0.initialized", mib, &miblen);
			for (i = ninitialized = 0; i < narenas; i++) {
				mib[1] = i;
				sz = sizeof(bool);
				xmallctlbymib(mib, miblen, &initialized[i], &sz,
				    NULL, 0);
				if (initialized[i]) {
					ninitialized++;
				}
			}
			mib[1] = MALLCTL_ARENAS_DESTROYED;
			sz = sizeof(bool);
			xmallctlbymib(mib, miblen, &destroyed_initialized, &sz,
			    NULL, 0);

			/* Merged stats. */
			if (merged && (ninitialized > 1 || !unmerged)) {
				/* Print merged arena stats. */
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t\"merged\": {\n");
				} else {
					malloc_cprintf(write_cb, cbopaque,
					    "\nMerged arenas stats:\n");
				}
				stats_arena_print(write_cb, cbopaque, json,
				    MALLCTL_ARENAS_ALL, bins, large, mutex);
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t}%s\n",
					    ((destroyed_initialized &&
					    destroyed) || unmerged) ?  "," :
					    "");
				}
			}

			/* Destroyed stats. */
			if (destroyed_initialized && destroyed) {
				/* Print destroyed arena stats. */
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t\"destroyed\": {\n");
				} else {
					malloc_cprintf(write_cb, cbopaque,
					    "\nDestroyed arenas stats:\n");
				}
				stats_arena_print(write_cb, cbopaque, json,
				    MALLCTL_ARENAS_DESTROYED, bins, large,
				    mutex);
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t}%s\n", unmerged ?  "," :
					    "");
				}
			}

			/* Unmerged stats. */
			if (unmerged) {
				for (i = j = 0; i < narenas; i++) {
					if (initialized[i]) {
						if (json) {
							j++;
							malloc_cprintf(write_cb,
							    cbopaque,
							    "\t\t\t\"%u\": {\n",
							    i);
						} else {
							malloc_cprintf(write_cb,
							    cbopaque,
							    "\narenas[%u]:\n",
							    i);
						}
						stats_arena_print(write_cb,
						    cbopaque, json, i, bins,
						    large, mutex);
						if (json) {
							malloc_cprintf(write_cb,
							    cbopaque,
							    "\t\t\t}%s\n", (j <
							    ninitialized) ? ","
							    : "");
						}
					}
				}
			}
		}

		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t}\n");
		}
	}
}

void
stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts) {
	int err;
	uint64_t epoch;
	size_t u64sz;
#define OPTION(o, v, d, s) bool v = d;
	STATS_PRINT_OPTIONS
#undef OPTION

	/*
	 * Refresh stats, in case mallctl() was called by the application.
	 *
	 * Check for OOM here, since refreshing the ctl cache can trigger
	 * allocation.  In practice, none of the subsequent mallctl()-related
	 * calls in this function will cause OOM if this one succeeds.
	 * */
	epoch = 1;
	u64sz = sizeof(uint64_t);
	err = je_mallctl("epoch", (void *)&epoch, &u64sz, (void *)&epoch,
	    sizeof(uint64_t));
	if (err != 0) {
		if (err == EAGAIN) {
			malloc_write("<jemalloc>: Memory allocation failure in "
			    "mallctl(\"epoch\", ...)\n");
			return;
		}
		malloc_write("<jemalloc>: Failure in mallctl(\"epoch\", "
		    "...)\n");
		abort();
	}

	if (opts != NULL) {
		for (unsigned i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
#define OPTION(o, v, d, s) case o: v = s; break;
				STATS_PRINT_OPTIONS
#undef OPTION
			default:;
			}
		}
	}

	emitter_t emitter;
	emitter_init(&emitter,
	    json ? emitter_output_json : emitter_output_table, write_cb,
	    cbopaque);
	emitter_begin(&emitter);
	emitter_table_note(&emitter, "___ Begin jemalloc statistics ___");
	emitter_json_dict_begin(&emitter, "jemalloc");

	if (general) {
		stats_general_print(&emitter, config_stats);
	}
	if (config_stats) {
		stats_print_helper(write_cb, cbopaque, json, merged, destroyed,
		    unmerged, bins, large, mutex);
	}

	emitter_json_dict_end(&emitter); /* Closes the "jemalloc" dict. */
	emitter_table_note(&emitter, "--- End jemalloc statistics ---");
	emitter_end(&emitter);
}
