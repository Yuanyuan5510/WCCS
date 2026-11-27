/* C core test matrix - section 5.1 of work.md (the only authoritative tests).
 * SPDX-License-Identifier: Apache-2.0 */
#include "wccs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do {                                              \
    ++g_checks;                                                            \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); ++g_fail; } \
} while (0)

#define RUN(name, fn) do { printf("[ %s ]\n", (name)); fn(); } while (0)

static void fill_ones(uint64_t *w, int n)
{
    int i;
    for (i = 0; i < n; ++i) w[i] = 1;
}

/* Reproducibility: two independent contexts with the same seed are identical. */
static void t_reproducibility(void)
{
    uint64_t w[10];
    int32_t e1 = 0, e2 = 0;
    wccs_ctx *a, *b;
    int i;
    fill_ones(w, 10);
    a = wccs_new_default(10, w, 3, 0, 0, 1000, 42, 0, &e1);
    b = wccs_new_default(10, w, 3, 0, 0, 1000, 42, 0, &e2);
    CHECK(a && b && e1 == 0 && e2 == 0, "construct two contexts");
    if (a && b) {
        for (i = 0; i < 2000; ++i) {
            int32_t x = wccs_sample(a, &e1);
            int32_t y = wccs_sample(b, &e2);
            CHECK(e1 == 0 && e2 == 0 && x == y && x >= 0 && x < 10,
                  "identical index sequence");
            if (g_fail > 5) break;
        }
    }
    wccs_free(a); wccs_free(b);
}

/* L=1, beta_q=0 must forbid consecutive identical indices. */
static void t_cooldown(void)
{
    uint64_t w[10];
    int32_t e = 0, prev = -1;
    wccs_ctx *c;
    int i;
    fill_ones(w, 10);
    c = wccs_new_default(10, w, 1, 0, 0, 1000, 99, 0, &e);
    CHECK(c && e == 0, "construct L=1");
    if (!c) return;
    for (i = 0; i < 20000; ++i) {
        int32_t r = wccs_sample(c, &e);
        CHECK(e == 0 && r >= 0 && r < 10, "valid sample");
        CHECK(r != prev, "no consecutive repeat");
        prev = r;
        if (g_fail > 5) break;
    }
    wccs_free(c);
}

/* L>=n, beta_q=0, equal weights => each round of n is a full permutation. */
static void t_coverage(void)
{
    uint64_t w[5];
    int32_t e = 0;
    wccs_ctx *c;
    int round, k, j;
    fill_ones(w, 5);
    c = wccs_new_default(5, w, 5, 0, 0, 1000, 123, 0, &e);
    CHECK(c && e == 0, "construct L=n");
    if (!c) return;
    for (round = 0; round < 50; ++round) {
        int seen[5] = {0, 0, 0, 0, 0};
        for (k = 0; k < 5; ++k) {
            int32_t r = wccs_sample(c, &e);
            CHECK(e == 0 && r >= 0 && r < 5, "valid sample");
            if (r >= 0 && r < 5) seen[r]++;
        }
        for (j = 0; j < 5; ++j)
            CHECK(seen[j] == 1, "each round covers the whole set exactly once");
        if (g_fail > 5) break;
    }
    wccs_free(c);
}

/* Unbiased mapping: L=0, equal weights, 1e6 draws per n. */
static void t_unbiased_for(int n)
{
    uint64_t w[16];
    long cnt[16] = {0};
    const long N = 1000000;
    long expect = N / n;
    int32_t e = 0;
    wccs_ctx *c;
    long i;
    int j;
    fill_ones(w, n);
    c = wccs_new_default(n, w, 0, 0, 0, 1000, 7, 0, &e);
    CHECK(c && e == 0, "construct unbiased");
    if (!c) return;
    for (i = 0; i < N; ++i) {
        int32_t r = wccs_sample(c, &e);
        if (r >= 0 && r < n) cnt[r]++;
        else CHECK(0, "index in range");
    }
    for (j = 0; j < n; ++j)
        CHECK(labs(cnt[j] - expect) < expect / 20, "uniform frequency (5%)");
    wccs_free(c);
}

static void t_unbiased(void)
{
    t_unbiased_for(3);
    t_unbiased_for(5);
    t_unbiased_for(7);
}

/* Weighted frequency follows w_i / sum(w). */
static void t_weighted_frequency(void)
{
    uint64_t w[4] = {1, 2, 3, 4};
    long cnt[4] = {0, 0, 0, 0};
    const long N = 1000000;
    int32_t e = 0;
    wccs_ctx *c;
    long i;
    int j;
    c = wccs_new_default(4, w, 0, 0, 0, 1000, 2024, 0, &e);
    CHECK(c && e == 0, "construct weighted");
    if (!c) return;
    for (i = 0; i < N; ++i) {
        int32_t r = wccs_sample(c, &e);
        if (r >= 0 && r < 4) cnt[r]++;
    }
    for (j = 0; j < 4; ++j)
        CHECK(labs(cnt[j] - N * (long)w[j] / 10) < N / 50,
              "weighted frequency within 2%");
    wccs_free(c);
}

/* Boundary / degeneracy table (section 1.6): must never crash. */
static void t_boundaries(void)
{
    uint64_t w1[1] = {1};
    uint64_t w3[3] = {1, 1, 1};
    uint64_t w4[4] = {1, 2, 3, 4};
    int32_t e = 0;
    wccs_ctx *c;
    int i;

    c = wccs_new_default(1, w1, 0, 0, 0, 1000, 1, 0, &e);
    CHECK(c && e == 0, "n=1 L=0 constructs");
    if (c) { for (i = 0; i < 100; ++i) CHECK(wccs_sample(c, &e) == 0 && e == 0, "n=1 L=0 always 0"); wccs_free(c); }

    e = 0;
    c = wccs_new_default(1, w1, 5, 0, 0, 1000, 1, 0, &e);
    CHECK(c && e == 0, "n=1 L>n constructs");
    if (c) { for (i = 0; i < 100; ++i) { int32_t r = wccs_sample(c, &e); CHECK(r == 0 && e == 0, "L>n recovers from total=0"); } wccs_free(c); }

    e = 0;
    c = wccs_new_default(4, w4, 2, (1ULL << 32), 0, 1000, 3, 0, &e);
    CHECK(c && e == 0, "beta_q = 2^32 (no cooldown) accepted");
    if (c) { for (i = 0; i < 1000; ++i) { int32_t r = wccs_sample(c, &e); CHECK(e == 0 && r >= 0 && r < 4, "beta_q=2^32 samples"); } wccs_free(c); }

    e = 0;
    c = wccs_new_default(3, w3, 10, 0, 0, 1000, 5, 0, &e);
    CHECK(c && e == 0, "L>n constructs");
    if (c) { for (i = 0; i < 30; ++i) { int32_t r = wccs_sample(c, &e); CHECK(e == 0 && r >= 0 && r < 3, "L>n samples"); } wccs_free(c); }

    /* total==0 path: n=3, L=3, beta_q=0 -> cold after 3 draws. */
    e = 0;
    c = wccs_new_default(3, w3, 3, 0, 0, 1000, 8, 0, &e);
    CHECK(c && e == 0, "total=0 case constructs");
    if (c) { for (i = 0; i < 12; ++i) { int32_t r = wccs_sample(c, &e); CHECK(e == 0 && r >= 0 && r < 3, "total=0 recovers"); } wccs_free(c); }
}

/* Error codes are stable and never crash (section 5.3). */
static void t_errors(void)
{
    uint64_t w[4] = {1, 2, 3, 4};
    uint64_t wz[4] = {1, 0, 3, 4};
    wccs_ctx *c;
    int32_t e;

    e = -1; c = wccs_new_default(0, w, 0, 0, 0, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "n=0 -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(-3, w, 0, 0, 0, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "n<0 -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(4, NULL, 0, 0, 0, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "NULL w -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(4, w, -1, 0, 0, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "L<0 -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(4, w, 0, 0, 0, 0, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "R=0 -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(4, w, 0, (1ULL << 32) + 1, 0, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "beta_q > 2^32 -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(4, w, 0, 0, (1ULL << 32) + 1, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "alpha_q > 2^32 -> E_INVAL"); wccs_free(c);
    e = -1; c = wccs_new_default(4, wz, 0, 0, 0, 1, 0, 0, &e);
    CHECK(c == NULL && e == WCCS_E_INVAL, "w_i=0 -> E_INVAL"); wccs_free(c);

    e = -1;
    CHECK(wccs_sample(NULL, &e) == -1 && e == WCCS_E_NULL, "NULL ctx -> E_NULL");
    wccs_free(NULL);  /* must be a no-op */

    CHECK(wccs_abi_version() == WCCS_ABI_VERSION, "ABI version is 1");
    CHECK(strlen(wccs_error_string(WCCS_E_INVAL)) > 0, "error string");
}

/* Credit rebuild path stays deterministic (alpha_q = 1, R = 10). */
static void t_credit_reproducibility(void)
{
    uint64_t w[8];
    int32_t e1 = 0, e2 = 0;
    wccs_ctx *a, *b;
    int i;
    fill_ones(w, 8);
    a = wccs_new_default(8, w, 2, 0, (1ULL << 32), 10, 555, 3, &e1);
    b = wccs_new_default(8, w, 2, 0, (1ULL << 32), 10, 555, 3, &e2);
    CHECK(a && b && e1 == 0 && e2 == 0, "construct with credit rebuild");
    if (a && b) {
        for (i = 0; i < 5000; ++i) {
            int32_t x = wccs_sample(a, &e1);
            int32_t y = wccs_sample(b, &e2);
            CHECK(e1 == 0 && e2 == 0 && x == y && x >= 0 && x < 8,
                  "credit path still deterministic");
            if (g_fail > 5) break;
        }
    }
    wccs_free(a); wccs_free(b);
}

/* 32-bit targets: n * sizeof(uint64_t) must not wrap size_t. */
static void t_capacity(void)
{
    uint64_t w[1] = {1};
    int32_t e = -1;
    wccs_ctx *c;
#if SIZE_MAX < UINT64_MAX
    c = wccs_new_default(600000000, w, 0, 0, 0, 1, 1, 0, &e);
    CHECK(c == NULL && e == WCCS_E_NOMEM, "oversized n is a clean capacity error");
    wccs_free(c);
#else
    (void)w; (void)e; (void)c;
    CHECK(1, "64-bit target: capacity guard is not reachable");
#endif
}

/* Read-only helpers. */
static void t_helpers(void)
{
    uint64_t w[4] = {1, 2, 3, 4};
    int32_t e = 0;
    wccs_ctx *c = wccs_new_default(4, w, 0, 0, 0, 1000, 1, 0, &e);
    CHECK(c != NULL, "construct helpers");
    if (!c) return;
    CHECK(wccs_step(c) == 0, "step starts at 0");
    CHECK(wccs_total(c) == 10, "total = sum(w)");
    CHECK(wccs_sample(c, &e) >= 0, "sample");
    CHECK(wccs_step(c) == 1, "step increments");
    CHECK(wccs_last_error(c) == WCCS_OK, "last error OK");
    CHECK(wccs_total(NULL) == 0 && wccs_step(NULL) == 0, "NULL getters");
    CHECK(wccs_last_error(NULL) == WCCS_E_NULL, "NULL last error");
    wccs_free(c);
}

/* Optional duplicate (seed, stream_id) detector - CONTRACT.md section 5.
 * Runs late and restores mode 0 so other tests may keep same-key contexts. */
static void t_stream_conflict(void)
{
    uint64_t w[4] = {1, 1, 1, 1};
    int32_t e = 0;
    wccs_ctx *a, *b;
    int32_t prev;

    prev = wccs_stream_check(2);
    CHECK(prev == 0, "detector is off by default");
    CHECK(wccs_stream_check_mode() == 2, "mode can be set");

    a = wccs_new_default(4, w, 0, 0, 0, 1, 7, 7, &e);
    CHECK(a != NULL && e == 0, "first key registers");

    e = 0;
    b = wccs_new_default(4, w, 0, 0, 0, 1, 7, 7, &e);
    CHECK(b == NULL && e == WCCS_E_STREAM_CONFLICT, "duplicate live key rejected");

    e = 0;
    b = wccs_new_default(4, w, 0, 0, 0, 1, 7, 8, &e);
    CHECK(b != NULL && e == 0, "distinct stream_id accepted");
    wccs_free(b);

    wccs_free(a);
    e = 0;
    a = wccs_new_default(4, w, 0, 0, 0, 1, 7, 7, &e);
    CHECK(a != NULL && e == 0, "key is reusable after wccs_free");
    wccs_free(a);

    wccs_stream_check(1);
    e = 0;
    a = wccs_new_default(4, w, 0, 0, 0, 1, 9, 9, &e);
    b = wccs_new_default(4, w, 0, 0, 0, 1, 9, 9, &e);
    CHECK(a != NULL && b != NULL, "warn mode never fails a construction");
    wccs_free(a);
    wccs_free(b);

    wccs_stream_check(0);
    CHECK(wccs_stream_check_mode() == 0, "detector restored to off");
}

int main(void)
{
    RUN("reproducibility",              t_reproducibility);
    RUN("cooldown violation",           t_cooldown);
    RUN("coverage",                     t_coverage);
    RUN("unbiased mapping",             t_unbiased);
    RUN("weighted frequency",           t_weighted_frequency);
    RUN("boundaries / degeneracy",      t_boundaries);
    RUN("error codes",                  t_errors);
    RUN("credit rebuild determinism",   t_credit_reproducibility);
    RUN("helpers",                      t_helpers);
    RUN("32-bit capacity guard",        t_capacity);
    RUN("stream conflict detector",     t_stream_conflict);
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
