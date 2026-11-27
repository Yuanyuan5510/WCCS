/* probe_duplicates.c -- empirical duplicate / design-flaw probe for WCCS.
 *
 * This file intentionally #includes ../src/wccs.c (instead of linking the
 * DLL) so that it can inspect internal state: cooldown[], recent[], cur[],
 * total. It never modifies the core; it only observes it.
 *
 * Experiment: for each configuration, run TRIALS independent samplers,
 * each producing DRAWS consecutive samples. Report the duplicate counts and
 * check hard invariants.
 *
 * Build (MinGW / gcc):
 *   gcc -O2 -std=c99 -DWCCS_STATIC -Iinclude tests/probe_duplicates.c \
 *       -o build/probe_duplicates.exe
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define WCCS_STATIC 1
/* MinGW's MSVCRT printf does not understand %lld; use MinGW's ANSI stdio. */
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include "../src/wccs.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TRIALS 1000
#define DRAWS  500
#define MAXN   512

/* ---------------- configuration matrix ---------------- */

typedef struct {
    const char *id;
    const char *wkind;      /* "equal" | "linear" | "spike" */
    const char *desc;
    int32_t     n, L, R;
    double      beta, alpha; /* Q32 factor as a real number in [0,1] */
} Cfg;

static uint64_t q32(double x) { return (uint64_t)(x * 4294967296.0 + 0.5); }

static const Cfg CFGS[] = {
    {"A0_pure",      "equal",  "L=0 pure weighted w/ replacement (baseline)", 10, 0, 1000, 0.0, 0.0},
    {"B1_noconsec",  "equal",  "L=1 beta=0 no consecutive repeats",          10, 1, 1000, 0.0, 0.0},
    {"C3_vector",    "equal",  "L=3 beta=0 fixed-vector config",             10, 3, 1000, 0.0, 0.0},
    {"D10_perm",     "equal",  "L=n beta=0 full permutation each round",     10,10, 1000, 0.0, 0.0},
    {"E25_Lgtn",     "equal",  "L>n beta=0 degeneracy / queue reset",        10,25, 1000, 0.0, 0.0},
    {"F_soft05",     "equal",  "L=3 beta=0.5 soft cooldown",                 10, 3, 1000, 0.5, 0.0},
    {"G_soft_credit","equal",  "L=3 beta=0.5 alpha=1 R=10",                  10, 3,   10, 0.5, 1.0},
    {"H_creditR1",   "equal",  "L=3 beta=0.5 alpha=1 R=1 (every draw)",      10, 3,    1, 0.5, 1.0},
    {"I_skew",       "linear", "w=1..10 L=3 beta=0",                         10, 3, 1000, 0.0, 0.0},
    {"J_spike",      "spike",  "one dominant weight, beta=0.5 alpha=1",      10, 3,  100, 0.5, 1.0},
    {"K_n500",       "equal",  "n=500 L=500 beta=0, 500 draws",             500,500,1000, 0.0, 0.0},
    {"L_small",      "equal",  "n=5 L=1 beta=0",                              5, 1, 1000, 0.0, 0.0},
    {"M_creditonly", "equal",  "L=0 alpha=1 R=10 credit only",               10, 0,   10, 0.0, 1.0},
    {"N_betaoff",    "equal",  "L=3 beta=1 (cooldown off)",                   10, 3, 1000, 1.0, 0.0},
    {"O_amplify",    "equal",  "n=3 L=20 beta=0.5 (stress repeated slots)",   3,20, 1000, 0.5, 0.0},
    {"P_beta09",     "equal",  "L=20 beta=0.9 soft, long window",            10,20, 1000, 0.9, 0.0},
    {"Q_creditR2",   "equal",  "L=3 beta=0.5 alpha=1 R=2",                   10, 3,    2, 0.5, 1.0},
    {"I2_skew_cred", "linear", "w=1..10 L=3 beta=0 alpha=1 R=10",            10, 3,   10, 0.0, 1.0},
    {"I3_skew_R1",   "linear", "w=1..10 L=3 beta=0 alpha=1 R=1",             10, 3,    1, 0.0, 1.0},
    {"I4_skew_soft", "linear", "w=1..10 L=3 beta=0.5 alpha=1 R=10",          10, 3,   10, 0.5, 1.0},
    {"R1_scaled",    "bigeq", "w=1000 each L=3 beta=0.5 (scaled control)",   10, 3, 1000, 0.5, 0.0},
    {"R2_scaled_cr", "bigeq", "w=1000 each L=3 beta=0.5 alpha=1 R=10",       10, 3,   10, 0.5, 1.0}
};
#define NCFG ((int)(sizeof(CFGS)/sizeof(CFGS[0])))

/* ---------------- accumulators ---------------- */

typedef struct {
    double s_dup, ss_dup;         /* 500 - distinct (duplicate "events")   */
    double s_pairs, ss_pairs;     /* sum C(count_i,2)                       */
    double s_adj, ss_adj;         /* consecutive repeats                    */
    double s_win, ss_win;         /* repeats with gap <= L                  */
    double s_dist, ss_dist;       /* distinct indices                       */
    double s_gap, ss_gap;         /* minimum repeat gap per trial           */
    long long gap_n;
    long long bad_win_trials;     /* trials with >=1 in-window repeat       */
    long long bad_adj_trials;     /* trials with >=1 consecutive repeat     */
    long long max_dup, max_win, min_gap;
    long long errors;             /* wccs_sample returned < 0               */
    long long zero_picks;         /* picked an index with v_i == 0          */
    long long cd_mismatch;        /* cooldown[] disagrees with recent[]     */
    long long tot_mismatch;       /* total != sum(cur)                      */
    long long resets;             /* total==0 queue-reset events            */
    long long dup_oldest;         /* oldest slot still present in new window */
    long long dup_slot;           /* window holds a repeated index          */
    long long reset_adj;          /* adjacent repeat right after a reset    */
    long long cooled_slots, cooled_zero; /* cooled indices with v_i == 0   */
    long long freq[MAXN];         /* cumulative pick counts per index       */
    long long draws_done;
} Agg;

static void agg_zero(Agg *a) { memset(a, 0, sizeof(*a)); a->min_gap = 1LL << 60; }

static void add(Agg *a, int idx, double v)
{
    double *s, *ss;
    switch (idx) {
        case 0: s=&a->s_dup;   ss=&a->ss_dup;   break;
        case 1: s=&a->s_pairs; ss=&a->ss_pairs; break;
        case 2: s=&a->s_adj;   ss=&a->ss_adj;   break;
        case 3: s=&a->s_win;   ss=&a->ss_win;   break;
        case 4: s=&a->s_dist;  ss=&a->ss_dist;  break;
        default:s=&a->s_gap;   ss=&a->ss_gap;   break;
    }
    *s += v; *ss += v*v;
}

static double mean(double s) { return s / (double)TRIALS; }
static double sd(double s, double ss)
{
    double m = s / (double)TRIALS;
    double v = ss / (double)TRIALS - m*m;
    return v > 0 ? sqrt(v) : 0.0;
}

/* ---------------- internal invariant probes ---------------- */

/* CONTRACT.md section 2 invariant: every index in the cooldown window must
 * carry beta_q, every index outside it ONE_Q32. Must stay zero for every
 * configuration; a non-zero value means the release path lost a reference. */
static unsigned char inwin[MAXN];
static int cd_check(const wccs_ctx *c)
{
    int L = c->L, count = c->recent_count, head = c->recent_head;
    uint64_t beta = c->beta_q;
    int start, k, mism = 0;

    if (L <= 0 || beta == ONE_Q32) return 0;
    memset(inwin, 0, (size_t)c->n);
    start = (head - count + L) % L;          /* oldest slot, count <= L */
    for (k = 0; k < count; ++k) inwin[c->recent[(start + k) % L]] = 1;
    for (k = 0; k < c->n; ++k) {
        uint64_t expect = inwin[k] ? beta : ONE_Q32;
        if (c->cooldown[k] != expect) ++mism;
    }
    return mism;
}

static int total_check(const wccs_ctx *c)
{
    uint64_t s = 0;
    int k;
    for (k = 0; k < c->n; ++k) s += c->cur[k];
    return s != c->total;
}

/* ---------------- main ---------------- */

static void build_weights(const Cfg *cfg, uint64_t *w)
{
    int i;
    for (i = 0; i < cfg->n; ++i) {
        if (!strcmp(cfg->wkind, "equal"))        w[i] = 1;
        else if (!strcmp(cfg->wkind, "bigeq"))   w[i] = 1000;
        else if (!strcmp(cfg->wkind, "linear"))  w[i] = (uint64_t)(i + 1);
        else                                     w[i] = (i == cfg->n - 1) ? 1000 : 1;
    }
}

int main(void)
{
    int ci, t, k, i, err;
    uint64_t w[MAXN], *pre_cur;
    int32_t seq[DRAWS], cnt[MAXN], last[MAXN];
    Agg a;
    double p_theory[MAXN];
    uint64_t Wtot;

    pre_cur = (uint64_t *)malloc(sizeof(uint64_t) * MAXN);
    if (!pre_cur) return 2;

    printf("{\n\"meta\": {\"trials\": %d, \"draws\": %d, \"seed_base\": 1000, \"abi\": %d},\n",
           TRIALS, DRAWS, wccs_abi_version());

    /* Effective dynamic weight of a cooled element:  v = (w * beta_q) >> 32.
     * For small integer weights this truncation makes "soft" cooldown hard. */
    {
        static const double betas[] = {0.0, 0.25, 0.5, 0.9, 1.0};
        static const uint64_t ws[]  = {1, 2, 10, 100, 1000};
        int bi, wi, first = 1;
        printf("\"beta_truncation\": [\n");
        for (wi = 0; wi < 5; ++wi) for (bi = 0; bi < 5; ++bi) {
            uint64_t v = mul128_shr32(ws[wi], q32(betas[bi]));
            printf("%s{\"w\":%llu,\"beta\":%.2f,\"v\":%llu}",
                   first ? "" : ",\n", (unsigned long long)ws[wi],
                   betas[bi], (unsigned long long)v);
            first = 0;
        }
        printf("\n],\n");
    }
    printf("\"configs\": [\n");

    for (ci = 0; ci < NCFG; ++ci) {
        const Cfg *cfg = &CFGS[ci];
        uint64_t beta_q = q32(cfg->beta), alpha_q = q32(cfg->alpha);
        agg_zero(&a);

        build_weights(cfg, w);
        Wtot = 0;
        for (i = 0; i < cfg->n; ++i) Wtot += w[i];
        for (i = 0; i < cfg->n; ++i) p_theory[i] = (double)w[i] / (double)Wtot;

        for (t = 0; t < TRIALS; ++t) {
            wccs_ctx *c = wccs_new_default(
                cfg->n, w, cfg->L, beta_q, alpha_q, cfg->R,
                (uint64_t)(1000 + t), (uint64_t)ci, &err);
            if (!c) { a.errors += DRAWS; continue; }

            memset(cnt, 0, sizeof(int32_t) * (size_t)cfg->n);
            for (i = 0; i < cfg->n; ++i) last[i] = -1;

            long long dup_events = 0, dup_pairs = 0, adj = 0, win = 0;
            long long distinct = 0, min_gap = 1LL << 60;

            for (k = 0; k < DRAWS; ++k) {
                int32_t idx;
                int was_reset = (c->total == 0);
                int old_count = c->recent_count, old_head = c->recent_head;
                int32_t old_oldest = -1;

                if (cfg->L > 0 && old_count > 0)
                    old_oldest = c->recent[(old_head - old_count + cfg->L) % cfg->L];

                memcpy(pre_cur, c->cur, sizeof(uint64_t) * (size_t)cfg->n);
                idx = wccs_sample(c, &err);
                if (idx < 0) { a.errors++; break; }

                a.draws_done++;
                a.freq[idx]++;
                if (!was_reset && pre_cur[idx] == 0) a.zero_picks++;
                if (was_reset) a.resets++;

                /* A full window means the oldest slot was just released. With
                 * soft cooldown that index may legitimately still appear in
                 * the new window; reference counting must keep it cooled.
                 * `cooldown_mismatch` is the real invariant check. */
                if (cfg->L > 0 && old_count == cfg->L &&
                    c->recent_count == cfg->L && old_oldest >= 0) {
                    int start = c->recent_head % cfg->L;
                    int appears = 0, m;
                    for (m = 0; m < cfg->L; ++m)
                        if (c->recent[(start + m) % cfg->L] == old_oldest) ++appears;
                    if (appears > 0) {
                        a.dup_slot++;
                        a.dup_oldest++;
                    }
                }

                /* How often a "soft" cooldown actually produces v_i == 0. */
                if (cfg->L > 0 && c->recent_count > 0) {
                    int start = (c->recent_head - c->recent_count + cfg->L) % cfg->L;
                    int m;
                    memset(inwin, 0, (size_t)cfg->n);
                    for (m = 0; m < c->recent_count; ++m)
                        inwin[c->recent[(start + m) % cfg->L]] = 1;
                    for (m = 0; m < cfg->n; ++m) if (inwin[m]) {
                        a.cooled_slots++;
                        if (c->cur[m] == 0) a.cooled_zero++;
                    }
                }

                a.cd_mismatch   += cd_check(c);
                a.tot_mismatch  += total_check(c);

                seq[k] = idx;
                if (was_reset && k > 0 && seq[k-1] == idx) a.reset_adj++;
                if (cnt[idx] == 0) distinct++;
                if (last[idx] >= 0) {
                    long long gap = k - last[idx];
                    dup_pairs += cnt[idx];
                    dup_events++;
                    if ((uint64_t)gap <= (uint64_t)cfg->L) win++;
                    if (gap < min_gap) min_gap = gap;
                }
                if (k > 0 && seq[k-1] == idx) adj++;
                cnt[idx]++;
                last[idx] = k;
            }

            add(&a, 0, (double)(DRAWS - distinct));   /* dup_events == 500-distinct */
            add(&a, 1, (double)dup_pairs);
            add(&a, 2, (double)adj);
            add(&a, 3, (double)win);
            add(&a, 4, (double)distinct);
            if (min_gap < (1LL << 60)) { add(&a, 5, (double)min_gap); a.gap_n++; }
            if (win > 0) a.bad_win_trials++;
            if (adj > 0) a.bad_adj_trials++;
            if (dup_events > a.max_dup) a.max_dup = dup_events;
            if (win > a.max_win) a.max_win = win;
            if (min_gap < a.min_gap) a.min_gap = min_gap;

            wccs_free(c);
        }

        /* chi-square of the aggregate frequency against base weights */
        double chi2 = 0.0, maxdev = 0.0;
        long long total_draws = 0;
        for (i = 0; i < cfg->n; ++i) total_draws += a.freq[i];
        for (i = 0; i < cfg->n; ++i) {
            double exp = p_theory[i] * (double)total_draws;
            double d = (double)a.freq[i] - exp;
            if (exp > 0) chi2 += d * d / exp;
            double rel = exp > 0 ? d / exp : 0.0;
            if (rel < 0) rel = -rel;
            if (rel > maxdev) maxdev = rel;
        }

        printf("%s{\"id\":\"%s\",\"desc\":\"%s\",\"n\":%d,\"w\":\"%s\","
               "\"L\":%d,\"beta\":%.3f,\"alpha\":%.3f,\"R\":%d,\n",
               ci ? ",\n" : "", cfg->id, cfg->desc, cfg->n, cfg->wkind,
               cfg->L, cfg->beta, cfg->alpha, cfg->R);
        printf(" \"dup_events_mean\":%.3f,\"dup_events_sd\":%.3f,\"dup_events_max\":%lld,\n",
               mean(a.s_dup), sd(a.s_dup, a.ss_dup), a.max_dup);
        printf(" \"dup_pairs_mean\":%.3f,\"adj_mean\":%.4f,\"in_window_repeat_mean\":%.4f,\n",
               mean(a.s_pairs), mean(a.s_adj), mean(a.s_win));
        long long mg = (a.min_gap == (1LL << 60)) ? -1 : a.min_gap;
        printf(" \"distinct_mean\":%.3f,\"min_gap_min\":%lld,\"min_gap_mean\":%.3f,\n",
               mean(a.s_dist), mg, a.gap_n ? a.s_gap/(double)a.gap_n : -1.0);
        printf(" \"trials_with_in_window_repeat\":%lld,\"trials_with_adj_repeat\":%lld,\n",
               a.bad_win_trials, a.bad_adj_trials);
        printf(" \"errors\":%lld,\"zero_weight_picks\":%lld,\"cooldown_mismatch\":%lld,"
               "\"total_mismatch\":%lld,\"queue_resets\":%lld,\n",
               a.errors, a.zero_picks, a.cd_mismatch, a.tot_mismatch, a.resets);
        printf(" \"dup_oldest_in_window\":%lld,\"dup_slot_events\":%lld,\"reset_adj\":%lld,"
               "\"cooled_zero_pct\":%.4f,\n",
               a.dup_oldest, a.dup_slot, a.reset_adj,
               a.cooled_slots ? 100.0 * (double)a.cooled_zero / (double)a.cooled_slots : 0.0);
        printf(" \"chi2\":%.2f,\"max_freq_rel_dev\":%.5f,\"expected_rebuilds_per_trial\":%d,",
               chi2, maxdev, DRAWS / cfg->R);
        printf("\"freq\":[");
        for (i = 0; i < cfg->n; ++i) printf("%s%lld", i ? "," : "", a.freq[i]);
        printf("]}");
    }
    printf("\n]\n}\n");

    free(pre_cur);
    return 0;
}
