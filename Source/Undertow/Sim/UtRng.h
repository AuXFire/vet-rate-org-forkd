// Deterministic random numbers (PCG32). Every simulation system draws from an Rng it is handed,
// so a given seed replays identically: that is what makes the headless tests and save/load reliable.
#pragma once

#include <cstdint>

namespace ut {

class Rng {
public:
    explicit Rng(uint64_t seed = 0x853c49e6748fea9bULL, uint64_t stream = 0xda3e39cb94b95bdbULL) { reseed(seed, stream); }

    void reseed(uint64_t seed, uint64_t stream = 0xda3e39cb94b95bdbULL) {
        state_ = 0U;
        inc_ = (stream << 1U) | 1U;
        nextU32();
        state_ += seed;
        nextU32();
    }

    uint32_t nextU32() {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18U) ^ old) >> 27U);
        const uint32_t rot = static_cast<uint32_t>(old >> 59U);
        return (xorshifted >> rot) | (xorshifted << ((32U - rot) & 31U));
    }

    // Uniform in [0, 1).
    float nextFloat() { return static_cast<float>(nextU32() >> 8U) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }
    // Uniform integer in [0, n). Returns 0 when n <= 0.
    int below(int n) { return n > 0 ? static_cast<int>(nextU32() % static_cast<uint32_t>(n)) : 0; }
    bool chance(float p) { return nextFloat() < p; }

    uint64_t state() const { return state_; }
    uint64_t stream() const { return inc_; }
    void restore(uint64_t state, uint64_t inc) { state_ = state; inc_ = inc; }

private:
    uint64_t state_ = 0U;
    uint64_t inc_ = 1U;
};

}  // namespace ut
