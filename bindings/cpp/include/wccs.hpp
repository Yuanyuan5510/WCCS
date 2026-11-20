/* WCCS C++ binding - thin RAII layer over the versioned C ABI.
 * No algorithm logic here: only type mapping, ownership and error translation.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "wccs.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wccs {

class Error : public std::runtime_error {
public:
    explicit Error(int32_t code)
        : std::runtime_error(std::string("wccs: ") + wccs_error_string(code)),
          code_(code) {}
    int32_t code() const noexcept { return code_; }
private:
    int32_t code_;
};

class Sampler {
public:
    Sampler(std::vector<uint64_t> weights,
            int32_t L, uint64_t beta_q, uint64_t alpha_q,
            int32_t R, uint64_t seed, uint64_t stream_id = 0)
        : ctx_(nullptr) {
        int32_t err = 0;
        ctx_ = wccs_new_default(static_cast<int32_t>(weights.size()),
                                weights.data(), L, beta_q, alpha_q,
                                R, seed, stream_id, &err);
        if (err != WCCS_OK) throw Error(err);
    }

    ~Sampler() { if (ctx_) wccs_free(ctx_); }

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&& other) noexcept : ctx_(other.ctx_) { other.ctx_ = nullptr; }
    Sampler& operator=(Sampler&& other) noexcept {
        if (this != &other) {
            if (ctx_) wccs_free(ctx_);
            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }
        return *this;
    }

    int32_t sample() {
        int32_t err = 0;
        int32_t r = wccs_sample(ctx_, &err);
        if (err != WCCS_OK) throw Error(err);
        return r;
    }

    uint64_t step()  const noexcept { return wccs_step(ctx_); }
    uint64_t total() const noexcept { return wccs_total(ctx_); }
    wccs_ctx* native_handle() noexcept { return ctx_; }

private:
    wccs_ctx* ctx_;
};

} // namespace wccs
