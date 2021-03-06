#include "test/jemalloc_test.h"

#define BATCH_MAX ((1U << 16) + 1024)
static void *ptrs[BATCH_MAX];

#define PAGE_ALIGNED(ptr) (((uintptr_t)ptr & PAGE_MASK) == 0)

static void
verify_stats(bin_stats_t *before, bin_stats_t *after, size_t batch,
    unsigned nregs) {
	if (!config_stats) {
		return;
	}
	if (config_prof && opt_prof) {
		/*
		 * Checking the stats when prof is on is feasible but
		 * complicated, while checking the non-prof case suffices for
		 * unit-test purpose.
		 */
		return;
	}
	expect_u64_eq(before->nmalloc + batch, after->nmalloc, "");
	expect_u64_eq(before->nrequests + batch, after->nrequests, "");
	expect_zu_eq(before->curregs + batch, after->curregs, "");
	size_t nslab = batch / nregs;
	size_t n_nonfull = 0;
	if (batch % nregs != 0) {
		++nslab;
		++n_nonfull;
	}
	expect_u64_eq(before->nslabs + nslab, after->nslabs, "");
	expect_zu_eq(before->curslabs + nslab, after->curslabs, "");
	expect_zu_eq(before->nonfull_slabs + n_nonfull, after->nonfull_slabs,
	    "");
}

static void
verify_batch(tsd_t *tsd, void **ptrs, size_t batch, size_t usize, bool zero,
    arena_t *arena, unsigned nregs) {
	for (size_t i = 0, j = 0; i < batch; ++i, ++j) {
		if (j == nregs) {
			j = 0;
		}
		void *p = ptrs[i];
		expect_zu_eq(isalloc(tsd_tsdn(tsd), p), usize, "");
		expect_ptr_eq(iaalloc(tsd_tsdn(tsd), p), arena, "");
		if (zero) {
			for (size_t k = 0; k < usize; ++k) {
				expect_true(*((unsigned char *)p + k) == 0, "");
			}
		}
		if (j == 0) {
			expect_true(PAGE_ALIGNED(p), "");
			continue;
		}
		assert(i > 0);
		void *q = ptrs[i - 1];
		bool adjacent = (uintptr_t)p > (uintptr_t)q
		    && (size_t)((uintptr_t)p - (uintptr_t)q) == usize;
		if (config_prof && opt_prof) {
			if (adjacent) {
				expect_false(prof_sampled(tsd, p)
				    || prof_sampled(tsd, q), "");
			} else {
				expect_true(prof_sampled(tsd, p)
				    || prof_sampled(tsd, q), "");
				expect_true(PAGE_ALIGNED(p), "");
				j = 0;
			}
		} else {
			expect_true(adjacent, "");
		}
	}
}

static void
release_batch(void **ptrs, size_t batch, size_t size) {
	for (size_t i = 0; i < batch; ++i) {
		sdallocx(ptrs[i], size, 0);
	}
}

typedef struct batch_alloc_packet_s batch_alloc_packet_t;
struct batch_alloc_packet_s {
	void **ptrs;
	size_t num;
	size_t size;
	int flags;
};

static size_t
batch_alloc_wrapper(void **ptrs, size_t num, size_t size, int flags) {
	batch_alloc_packet_t batch_alloc_packet = {ptrs, num, size, flags};
	size_t filled;
	size_t len = sizeof(size_t);
	assert_d_eq(mallctl("experimental.batch_alloc", &filled, &len,
	    &batch_alloc_packet, sizeof(batch_alloc_packet)), 0, "");
	return filled;
}

static void
test_wrapper(size_t size, size_t alignment, bool zero, unsigned arena_flag) {
	tsd_t *tsd = tsd_fetch();
	assert(tsd != NULL);
	const size_t usize =
	    (alignment != 0 ? sz_sa2u(size, alignment) : sz_s2u(size));
	const szind_t ind = sz_size2index(usize);
	const bin_info_t *bin_info = &bin_infos[ind];
	const unsigned nregs = bin_info->nregs;
	assert(nregs > 0);
	arena_t *arena;
	if (arena_flag != 0) {
		arena = arena_get(tsd_tsdn(tsd), MALLOCX_ARENA_GET(arena_flag),
		    false);
	} else {
		arena = arena_choose(tsd, NULL);
	}
	assert(arena != NULL);
	bin_t *bin = arena_bin_choose(tsd_tsdn(tsd), arena, ind, NULL);
	assert(bin != NULL);
	int flags = arena_flag;
	if (alignment != 0) {
		flags |= MALLOCX_ALIGN(alignment);
	}
	if (zero) {
		flags |= MALLOCX_ZERO;
	}

	/*
	 * Allocate for the purpose of bootstrapping arena_tdata, so that the
	 * change in bin stats won't contaminate the stats to be verified below.
	 */
	void *p = mallocx(size, flags | MALLOCX_TCACHE_NONE);

	for (size_t i = 0; i < 4; ++i) {
		size_t base = 0;
		if (i == 1) {
			base = nregs;
		} else if (i == 2) {
			base = nregs * 2;
		} else if (i == 3) {
			base = (1 << 16);
		}
		for (int j = -1; j <= 1; ++j) {
			if (base == 0 && j == -1) {
				continue;
			}
			size_t batch = base + (size_t)j;
			assert(batch < BATCH_MAX);
			bin_stats_t stats_before, stats_after;
			memcpy(&stats_before, &bin->stats, sizeof(bin_stats_t));
			size_t filled = batch_alloc_wrapper(ptrs, batch, size,
			    flags);
			assert_zu_eq(filled, batch, "");
			memcpy(&stats_after, &bin->stats, sizeof(bin_stats_t));
			verify_stats(&stats_before, &stats_after, batch, nregs);
			verify_batch(tsd, ptrs, batch, usize, zero, arena,
			    nregs);
			release_batch(ptrs, batch, usize);
		}
	}

	free(p);
}

TEST_BEGIN(test_batch_alloc) {
	test_wrapper(11, 0, false, 0);
}
TEST_END

TEST_BEGIN(test_batch_alloc_zero) {
	test_wrapper(11, 0, true, 0);
}
TEST_END

TEST_BEGIN(test_batch_alloc_aligned) {
	test_wrapper(7, 16, false, 0);
}
TEST_END

TEST_BEGIN(test_batch_alloc_manual_arena) {
	unsigned arena_ind;
	size_t len_unsigned = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.create", &arena_ind, &len_unsigned, NULL,
	    0), 0, "");
	test_wrapper(11, 0, false, MALLOCX_ARENA(arena_ind));
}
TEST_END

TEST_BEGIN(test_batch_alloc_fallback) {
	const size_t size = SC_LARGE_MINCLASS;
	for (size_t batch = 0; batch < 4; ++batch) {
		assert(batch < BATCH_MAX);
		size_t filled = batch_alloc(ptrs, batch, size, 0);
		assert_zu_eq(filled, batch, "");
		release_batch(ptrs, batch, size);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_batch_alloc,
	    test_batch_alloc_zero,
	    test_batch_alloc_aligned,
	    test_batch_alloc_manual_arena,
	    test_batch_alloc_fallback);
}
