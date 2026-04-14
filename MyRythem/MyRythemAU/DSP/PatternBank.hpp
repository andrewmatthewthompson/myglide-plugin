#pragma once
//
// PatternBank — rhythmic step patterns for MyRythem.
//
// A pattern is a fixed-length bitmask (up to 32 steps). Each step that is
// "on" causes the held chord to be re-triggered. The base step duration
// is determined by the Rate parameter (independent of the pattern shape).
//
// Built-in patterns are expressed on a 16-step-per-bar grid (1/16 notes).
// At 1/8 Rate the pattern is time-scaled to half speed automatically by
// the step clock — the bitmask itself does not change.
//
// CUSTOM slot is writable at runtime and backed by a uint32_t mask + length.
//

#include <cstdint>

namespace myrythem {

enum class PatternId : int {
    Straight8ths       = 0,
    Straight16ths      = 1,
    Dotted8ths         = 2,
    EighthSixteenth    = 3,   // ♪ ♬  ♪ ♬  ("8 + 16" feel)
    SixteenthEighth    = 4,   // ♬ ♪  ♬ ♪
    Dotted8thPlus16th  = 5,   // 3/16 + 1/16 repeating
    EighthTriplets     = 6,
    Shuffle8ths        = 7,   // straight 8ths, swing param bends the grid
    QuarterPulse       = 8,
    Custom             = 9,
    Count              = 10
};

struct Pattern {
    uint32_t mask;     // bit i (LSB = step 0) set if step i triggers
    uint8_t  length;   // number of steps (1..32)
    uint8_t  grid;     // native grid: 2=8th, 4=16th, 3=triplet8th
};

// -----------------------------------------------------------------------------
// Built-in patterns (all expressed at 16 steps/bar unless noted).
// A "1" means emit the chord; a "0" means the step is silent.
//
// Bit 0 = downbeat of the bar.
// -----------------------------------------------------------------------------

//                                                  step: 0 1 2 3 4 5 6 7 8 9 A B C D E F
inline constexpr uint32_t kMaskStraight8ths      = 0b0101010101010101u;  // every 8th
inline constexpr uint32_t kMaskStraight16ths     = 0b1111111111111111u;  // every 16th
inline constexpr uint32_t kMaskQuarterPulse      = 0b0001000100010001u;  // every quarter

// Dotted 8th = 3/16. In 16 steps: 0, 3, 6, 9, 12, 15 (6 hits spanning 18/16, wraps).
// We use the sub-pattern that repeats cleanly within 16 steps: 0, 3, 6, 9, 12.
// Real-world bar: dotted 8ths don't tile evenly into 4/4 — we take the repeating
// 3/16 grid and let the scheduler wrap. Actual implementation uses
// length=3 at 16th grid so the pattern repeats every 3 sixteenths.
inline constexpr uint32_t kMaskDotted8ths        = 0b001u;               // length 3
inline constexpr uint32_t kMaskDotted8thPlus16th = 0b1001u;              // length 4 (3/16 + 1/16)

// 8th + 16th: "long-short" inside each beat.
//   Beat:  ♪   ♬       ♪   ♬
//   16ths: 1 0 1 1     1 0 1 1  ...
inline constexpr uint32_t kMaskEighthSixteenth   = 0b1101110111011101u;
inline constexpr uint32_t kMaskSixteenthEighth   = 0b1011101110111011u;  // "short-long"

// 8th triplets: 12 steps per bar, all active.
inline constexpr uint32_t kMaskEighthTriplets    = 0b111111111111u;      // length 12, triplet grid

// Shuffle 8ths: same as straight 8ths, swing param adds per-step delay.
inline constexpr uint32_t kMaskShuffle8ths       = 0b0101010101010101u;

inline Pattern builtinPattern(PatternId id) {
    switch (id) {
        case PatternId::Straight8ths:      return { kMaskStraight8ths,      16, 4 };
        case PatternId::Straight16ths:     return { kMaskStraight16ths,     16, 4 };
        case PatternId::Dotted8ths:        return { kMaskDotted8ths,         3, 4 };
        case PatternId::EighthSixteenth:   return { kMaskEighthSixteenth,   16, 4 };
        case PatternId::SixteenthEighth:   return { kMaskSixteenthEighth,   16, 4 };
        case PatternId::Dotted8thPlus16th: return { kMaskDotted8thPlus16th,  4, 4 };
        case PatternId::EighthTriplets:    return { kMaskEighthTriplets,    12, 3 };
        case PatternId::Shuffle8ths:       return { kMaskShuffle8ths,       16, 4 };
        case PatternId::QuarterPulse:      return { kMaskQuarterPulse,      16, 4 };
        case PatternId::Custom:            return { 0u,                     16, 4 };
        default:                           return { kMaskEighthSixteenth,   16, 4 };
    }
}

/// True if step index `stepInBar` of the given pattern fires.
inline bool stepActive(const Pattern& p, int stepInBar) {
    if (p.length == 0) return false;
    int idx = stepInBar % p.length;
    return (p.mask >> idx) & 1u;
}

}  // namespace myrythem
