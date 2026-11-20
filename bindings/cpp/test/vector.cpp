/* C++ binding smoke test + cross-language vector emitter.
 * Output line "indices: ..." is consumed by tests/run_cross_lang.py.
 * SPDX-License-Identifier: Apache-2.0 */
#include "wccs.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

int main()
{
    const std::vector<uint64_t> w(10, 1);
    try {
        wccs::Sampler s(w, 3, 0, 0, 1000, 42, 0);
        if (s.step() != 0 || s.total() != 10) {
            std::printf("ERROR: unexpected initial state\n");
            return 1;
        }
        std::printf("indices:");
        for (int i = 0; i < 100; ++i) {
            int32_t r = s.sample();
            if (r < 0 || r >= 10) { std::printf("\nERROR: out of range\n"); return 1; }
            std::printf(" %d", (int)r);
        }
        std::printf("\n");
        if (s.step() != 100) { std::printf("ERROR: step != 100\n"); return 1; }

        /* Error translation must be an exception, never a crash. */
        bool threw = false;
        try {
            wccs::Sampler bad(std::vector<uint64_t>{1, 0, 1}, 0, 0, 0, 1, 0, 0);
        } catch (const wccs::Error& e) {
            threw = (e.code() == WCCS_E_INVAL);
        }
        if (!threw) { std::printf("ERROR: no exception on invalid input\n"); return 1; }
    } catch (const wccs::Error& e) {
        std::printf("ERROR: %s\n", e.what());
        return 1;
    }
    return 0;
}
