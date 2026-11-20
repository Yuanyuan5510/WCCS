/* Minimal self-contained SHA-256 used only by the test vector tooling.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef WCCS_TEST_SHA256_H
#define WCCS_TEST_SHA256_H
#include <stdint.h>
#include <stddef.h>
void wccs_sha256_hex(const uint8_t *data, size_t len, char out_hex[65]);
#endif
