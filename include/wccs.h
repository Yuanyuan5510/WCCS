/* WCCS - Weighted Credit-Cooldown Sampler
 * Versioned C ABI. This header is the single stable contract (see docs/ABI.md).
 * Behavioural rulings for cases the manual leaves undefined or contradictory
 * live in docs/CONTRACT.md; they are authoritative over work.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ABI rules:
 *   - Append only. Never change an existing signature or enum value.
 *   - Opaque handle + constructor/destructor; internal layout is never exposed.
 *   - Memory allocated by the core is released by the core (wccs_free).
 */
#ifndef WCCS_H
#define WCCS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WCCS_ABI_VERSION 1

/* Stable error codes. Append new codes at the end; never renumber. */
enum {
    WCCS_OK         = 0,
    WCCS_E_INVAL    = 1,  /* invalid argument (n<=0, w=NULL, w_i=0, L<0, R<1,
                             beta_q/alpha_q > 2^32, missing rng callback)   */
    WCCS_E_NOMEM    = 2,  /* allocation failure                              */
    WCCS_E_OVERFLOW = 3,  /* sum of weights exceeds uint64                    */
    WCCS_E_EMPTY    = 4,  /* all dynamic weights are zero; cannot sample      */
    WCCS_E_NULL     = 5,  /* NULL context passed to a method                  */
    WCCS_E_STATE    = 6,  /* internal invariant violated (defensive)          */
    WCCS_E_STREAM_CONFLICT = 7  /* two live contexts share (seed, stream_id);
                                   only reported when the optional check is on */
};

#if defined(_WIN32) && !defined(WCCS_STATIC)
#  if defined(WCCS_BUILD_DLL)
#    define WCCS_API __declspec(dllexport)
#  else
#    define WCCS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define WCCS_API __attribute__((visibility("default")))
#else
#  define WCCS_API
#endif

typedef struct wccs_ctx wccs_ctx;

/* Injected random source. The core never owns the RNG state unless the
 * convenience constructor wccs_new_default() is used. */
typedef struct {
    void     *ctx;
    uint64_t (*next_u64)(void *ctx);
} wccs_rng_t;

/* Built-in deterministic reference PRNG (PCG RXS-M-XS 64/64).
 * All language bindings MUST share this engine for L2 reproducibility;
 * the easiest way is to call wccs_new_default(). */
typedef struct {
    uint64_t state;
} wccs_pcg64_t;

WCCS_API void     wccs_pcg64_init(wccs_pcg64_t *p, uint64_t seed, uint64_t stream_id);
WCCS_API uint64_t wccs_pcg64_next(void *ctx);

/* Primary constructor with custom RNG injection (original ABI 1 contract).
 * seed / stream_id are recorded as metadata in this mode; the RNG is fully
 * supplied by the caller. */
WCCS_API wccs_ctx* wccs_new(
    int32_t n, const uint64_t *w,
    int32_t L, uint64_t beta_q, uint64_t alpha_q,
    int32_t R, uint64_t seed, uint64_t stream_id,
    wccs_rng_t rng, int32_t *err);

/* Convenience constructor added in ABI 1: uses the built-in reference PCG64
 * initialised from (seed, stream_id). Every binding that calls this produces
 * the byte-identical index sequence across languages and platforms. */
WCCS_API wccs_ctx* wccs_new_default(
    int32_t n, const uint64_t *w,
    int32_t L, uint64_t beta_q, uint64_t alpha_q,
    int32_t R, uint64_t seed, uint64_t stream_id,
    int32_t *err);

WCCS_API int32_t wccs_sample(wccs_ctx *c, int32_t *err);
WCCS_API void    wccs_free(wccs_ctx *c);
WCCS_API int32_t wccs_abi_version(void);

/* Helpers appended in ABI 1 (read-only / static storage). */
WCCS_API const char* wccs_error_string(int32_t err); /* static string, DO NOT free */
WCCS_API uint64_t    wccs_step(const wccs_ctx *c);
WCCS_API uint64_t    wccs_total(const wccs_ctx *c);
WCCS_API int32_t     wccs_last_error(const wccs_ctx *c);

/* Optional debug aid (ABI 1, append-only). Disabled by default.
 * Two simultaneously live contexts constructed with the same
 * (seed, stream_id) produce identical sequences: intentional for L2
 * reproducibility tests, a silent bug in parallel runs.
 *
 *   mode 0 = off (default), 1 = warn on stderr, 2 = fail construction
 *   with WCCS_E_STREAM_CONFLICT.
 *
 * Returns the previous mode. Set it before creating any context; the
 * toggle itself is not thread-safe. See docs/CONTRACT.md section 5. */
WCCS_API int32_t wccs_stream_check(int32_t mode);
WCCS_API int32_t wccs_stream_check_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* WCCS_H */
