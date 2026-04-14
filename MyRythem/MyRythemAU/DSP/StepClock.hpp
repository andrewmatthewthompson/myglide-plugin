#pragma once
//
// StepClock — converts host beat position into step-phase, with swing.
//
// A "step" is one slot of the pattern. Its musical length is:
//     stepBeats = 1 / rateSubdivisionsPerBeat
//
//   Rate            subdivisionsPerBeat  stepBeats
//   --------------  -------------------  ---------
//   1/4                     1             1.0
//   1/8                     2             0.5
//   1/8 triplet             3             0.333...
//   1/16                    4             0.25
//   1/16 triplet            6             0.1666...
//   1/32                    8             0.125
//
// The clock reports:
//   - stepIndex       : which step in the bar we are on (0..∞, wraps mod length)
//   - positionInStep  : [0,1) where in the current step we are (before swing)
//   - justCrossed     : true on the sample-block where we crossed a step boundary
//
// Swing (50..75 %): even-indexed steps fire on time, odd-indexed steps fire
// later by `(swing-50)/50 * 0.5 * stepBeats`. 50 % = straight, 66.67 % = triplet,
// 75 % = dotted.
//

#include <cstdint>
#include <cmath>

namespace myrythem {

enum class RateId : int {
    Quarter         = 0,   // 1/4
    Eighth          = 1,   // 1/8
    EighthTriplet   = 2,   // 1/8T
    Sixteenth       = 3,   // 1/16
    SixteenthTriplet= 4,   // 1/16T
    ThirtySecond    = 5,   // 1/32
    Count           = 6
};

inline double subdivisionsPerBeat(RateId r) {
    switch (r) {
        case RateId::Quarter:          return 1.0;
        case RateId::Eighth:           return 2.0;
        case RateId::EighthTriplet:    return 3.0;
        case RateId::Sixteenth:        return 4.0;
        case RateId::SixteenthTriplet: return 6.0;
        case RateId::ThirtySecond:     return 8.0;
        default:                       return 4.0;
    }
}

class StepClock {
public:
    void reset() {
        mLastStepIndex = -1;
        mJustCrossed   = false;
    }

    /// Update internal state from the current host beat position.
    /// Call once per render block. `beatPos` is the transport beat at the
    /// start of the block. Returns true if we crossed into a new step
    /// this block (i.e. the scheduler should fire this step).
    bool update(double beatPos, RateId rate, float swingPct /* 50..75 */) {
        double stepBeats = 1.0 / subdivisionsPerBeat(rate);
        if (stepBeats <= 0.0) stepBeats = 0.25;

        // Apply swing offset. Only odd steps are delayed. We compute the
        // "swung" beat position by subtracting an offset for odd steps.
        // But since we don't yet know which step we're on until we compute
        // it, do a two-pass: compute nominal step, then if odd, subtract
        // swing offset and re-check.
        double rawSteps = beatPos / stepBeats;
        int64_t nominal = (int64_t)std::floor(rawSteps);

        double swingFrac = ((double)swingPct - 50.0) / 50.0;  // 0..0.5
        if (swingFrac < 0.0) swingFrac = 0.0;
        if (swingFrac > 0.5) swingFrac = 0.5;
        double swingBeats = swingFrac * 0.5 * stepBeats;

        int64_t stepIndex = nominal;
        if (swingBeats > 0.0 && (nominal & 1) == 1) {
            // Odd step: it should only be "entered" once beatPos has passed
            // (stepBeats * nominal) + swingBeats.
            double enterBeat = stepBeats * (double)nominal + swingBeats;
            if (beatPos < enterBeat) {
                stepIndex = nominal - 1;  // still on previous (even) step
            }
        }

        mJustCrossed = (stepIndex != mLastStepIndex);
        mLastStepIndex = stepIndex;
        mStepBeats = stepBeats;
        mCurrentStepIndex = stepIndex;

        // Position in step [0,1)
        double baseBeat = stepBeats * (double)stepIndex;
        if ((stepIndex & 1) == 1) baseBeat += swingBeats;
        double effStepLen = stepBeats;  // swing doesn't change length, just start
        double inStep = (beatPos - baseBeat) / effStepLen;
        if (inStep < 0.0) inStep = 0.0;
        if (inStep > 1.0) inStep = 1.0;
        mPositionInStep = inStep;

        return mJustCrossed;
    }

    /// Force the internal "last step" counter to `idx` without triggering.
    /// Used on transport locate/loop so we don't double-fire.
    void syncTo(int64_t idx) {
        mLastStepIndex    = idx;
        mCurrentStepIndex = idx;
        mJustCrossed      = false;
    }

    int64_t stepIndex() const      { return mCurrentStepIndex; }
    double  positionInStep() const { return mPositionInStep; }
    double  stepBeats() const      { return mStepBeats; }
    bool    justCrossed() const    { return mJustCrossed; }

private:
    int64_t mLastStepIndex    = -1;
    int64_t mCurrentStepIndex = 0;
    double  mPositionInStep   = 0.0;
    double  mStepBeats        = 0.25;
    bool    mJustCrossed      = false;
};

}  // namespace myrythem
