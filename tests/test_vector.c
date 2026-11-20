/* Fixed cross-language test vector (section 5.2 of work.md).
 *
 * Vector: seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0,
 *         R=1000, first 100 sampled indices.
 * Digest: SHA-256 over the 100 indices encoded as 100 consecutive uint8
 *         bytes (little sequence, one byte per index, no separators).
 *
 * This program is the C reference. Every language binding must reproduce
 * the same digest. SPDX-License-Identifier: Apache-2.0
 */
#include "wccs.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint64_t w[10];
    uint8_t seq[100];
    int32_t err = 0;
    wccs_ctx *c;
    char hex[65];
    int i;

    for (i = 0; i < 10; ++i) w[i] = 1;
    c = wccs_new_default(10, w, 3, 0, 0, 1000, 42, 0, &err);
    if (!c) {
        printf("ERROR: init failed: %s\n", wccs_error_string(err));
        return 1;
    }
    for (i = 0; i < 100; ++i) {
        int32_t r = wccs_sample(c, &err);
        if (err != WCCS_OK || r < 0 || r >= 10) {
            printf("ERROR: sample %d failed: %s\n", i, wccs_error_string(err));
            wccs_free(c);
            return 1;
        }
        seq[i] = (uint8_t)r;
    }
    wccs_free(c);

    wccs_sha256_hex(seq, sizeof(seq), hex);

    printf("vector: seed=42 stream_id=0 n=10 w=all1 L=3 beta_q=0 alpha_q=0 R=1000 count=100\n");
    printf("indices:");
    for (i = 0; i < 100; ++i) printf(" %u", (unsigned)seq[i]);
    printf("\n");
    printf("digest: SHA256(100 x uint8 indices)\n");
    printf("sha256: %s\n", hex);

    /* Self-check: a regression in the core changes the digest and fails ctest. */
    if (strcmp(hex,
        "30776469ff25ee678e297c43e0bdaadb352340f6b53fcb0587d091209f08ba68") != 0) {
        printf("ERROR: digest does not match the pinned reference vector\n");
        return 1;
    }
    return 0;
}
