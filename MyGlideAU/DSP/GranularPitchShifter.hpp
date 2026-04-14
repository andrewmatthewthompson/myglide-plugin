#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

/// Two-tap crossfaded varispeed delay line — the "pitch bend wheel"
/// architecture.
///
/// Unlike a granular / phase-vocoder pitch shifter, this does NOT
/// time-stretch. The output is the input played back at a variable rate
/// out of a delay line, which is exactly what a synth pitch bend, a
/// vinyl slowing down, or a tape varispeed does. That means:
///
///   • At ratio 1.0 there is zero drift and the output is a clean,
///     constant-delay copy of the input. No Hann windowing, no
///     overlap-add, no artefacts.
///   • During a pitch glide the single active read head traces the
///     buffer at the pitch ratio. The sound is a smooth, continuous
///     pitch bend with the tonal quality of the input preserved —
///     there is no grain window modulating the signal.
///   • Two crossfaded read taps rescue the read position when the
///     drift from the write head gets too large (pitch down) or too
///     small (pitch up), so sustained non-unity pitches don't run out
///     of buffer. The crossfade uses a constant-power sin/cos curve so
///     the handover is inaudible on tonal material.
///
/// RT-safe: all memory is external; no allocations in `process()`.
///
/// API stability: the `configure` signature, `setPitchSemitones`,
/// `process`, `writeInputOnly`, and `pitchRatio()` methods match the
/// previous Hann-grain implementation so GlideProcessor doesn't need
/// to change. The `hannPtr` and `grainSamples` arguments are repurposed
/// — `grainSamples` is now the target delay at rest (matching the AU's
/// reported latencySamples()), and `hannPtr` is ignored.
class GranularPitchShifter {
public:
    GranularPitchShifter() = default;
    GranularPitchShifter(const GranularPitchShifter&) = delete;
    GranularPitchShifter& operator=(const GranularPitchShifter&) = delete;

    void configure(double* bufferPtr, int32_t bufferSize,
                   double* /*hannPtr — unused, kept for API stability*/,
                   int32_t grainSamples, double sampleRate) {
        mBuffer     = bufferPtr;
        mBufferSize = bufferSize;
        mSampleRate = sampleRate;

        if (!mBuffer || mBufferSize <= 0) return;

        std::memset(mBuffer, 0, sizeof(double) * mBufferSize);

        // Target delay (resting state, when ratio == 1.0). Matches the
        // AU's latencySamples() so Logic Pro's latency compensation
        // aligns the output with real time.
        mTargetDelay = std::max(4.0, static_cast<double>(grainSamples));

        // Drift bounds. When the active tap's delay drops below
        // `mMinDelay` (pitch up, catching the write head) or exceeds
        // `mMaxDelay` (pitch down, falling too far behind), we
        // crossfade to a fresh tap at the target delay. The bounds are
        // generous so short bends never trigger a crossfade.
        mMinDelay = std::max(2.0, 0.25 * mTargetDelay);
        mMaxDelay = std::min(static_cast<double>(mBufferSize - 4),
                             6.0 * mTargetDelay);

        // 15 ms equal-power crossfade is short enough to be inaudible
        // on most material but long enough to avoid clicks.
        double xfadeSamples = std::max(64.0, 0.015 * mSampleRate);
        mXfadeInc = 1.0 / xfadeSamples;

        mWritePos = 0;
        mPitchRatio = 1.0;
        mLastSemitones = 0.0;

        // Initialise both read taps at the same safe position
        // `mTargetDelay` samples behind the write head. Tap A is
        // primary, tap B is dormant until the first crossfade.
        double startPos = -mTargetDelay;
        if (startPos < 0) startPos += mBufferSize;
        mReadPosA   = startPos;
        mReadPosB   = startPos;
        mWeightA    = 1.0;
        mIsXfading  = false;
        mFadingToB  = false;
        mXfadeProgress = 0.0;
    }

    /// Set pitch shift in semitones. Clamped to ±48.
    void setPitchSemitones(double semitones) {
        if (semitones > 48.0) semitones = 48.0;
        if (semitones < -48.0) semitones = -48.0;
        if (std::fabs(semitones - mLastSemitones) > 0.001) {
            mPitchRatio = std::pow(2.0, semitones / 12.0);
            mLastSemitones = semitones;
        }
    }

    /// Feed the buffer without performing any read (used by the idle
    /// path in GlideProcessor — currently unused since that bypass was
    /// removed but kept available for future optimisation).
    void writeInputOnly(double input) {
        if (!mBuffer || mBufferSize == 0) return;
        mBuffer[mWritePos] = input;
        mWritePos = (mWritePos + 1) % mBufferSize;
    }

    /// Process a single sample. Writes input, returns pitch-shifted
    /// output with a `mTargetDelay`-sample latency (compensated by the
    /// host via `latencySamples()`).
    double process(double input) {
        if (!mBuffer || mBufferSize == 0) return input;

        // Write input into the ring buffer.
        mBuffer[mWritePos] = input;
        mWritePos = (mWritePos + 1) % mBufferSize;

        // Read both taps with cubic Hermite interpolation.
        const double sampleA = hermiteRead(mReadPosA);
        const double sampleB = hermiteRead(mReadPosB);
        const double output  = sampleA * mWeightA + sampleB * (1.0 - mWeightA);

        // Advance both read heads at the pitch rate. At ratio 1.0 the
        // head advances in lockstep with the write head so the delay
        // stays constant; ratio > 1 makes it catch up, ratio < 1 makes
        // it fall behind.
        const double bufSize = static_cast<double>(mBufferSize);
        mReadPosA += mPitchRatio;
        if (mReadPosA >= bufSize)     mReadPosA -= bufSize;
        else if (mReadPosA < 0.0)     mReadPosA += bufSize;

        mReadPosB += mPitchRatio;
        if (mReadPosB >= bufSize)     mReadPosB -= bufSize;
        else if (mReadPosB < 0.0)     mReadPosB += bufSize;

        // Progress an in-flight crossfade, or trigger a new one if the
        // active tap has drifted out of the safe range.
        updateCrossfade();

        return output;
    }

    double pitchRatio() const { return mPitchRatio; }

private:
    void updateCrossfade() {
        if (mIsXfading) {
            mXfadeProgress += mXfadeInc;
            if (mXfadeProgress >= 1.0) {
                mXfadeProgress = 1.0;
                mIsXfading = false;
                mWeightA = mFadingToB ? 0.0 : 1.0;
            } else {
                const double t = mXfadeProgress;
                // Constant-power (equal-power) crossfade.
                if (mFadingToB) {
                    mWeightA = std::cos(t * M_PI * 0.5);   // 1 → 0
                } else {
                    mWeightA = std::sin(t * M_PI * 0.5);   // 0 → 1
                }
            }
            return;
        }

        // Not currently crossfading — check whether we need to start one.
        const bool primaryIsA = (mWeightA > 0.5);
        const double primaryPos = primaryIsA ? mReadPosA : mReadPosB;
        const double delay = readDelay(primaryPos);

        if (delay < mMinDelay || delay > mMaxDelay) {
            double newPos = static_cast<double>(mWritePos) - mTargetDelay;
            if (newPos < 0.0) newPos += static_cast<double>(mBufferSize);

            if (primaryIsA) {
                mReadPosB  = newPos;
                mFadingToB = true;   // weightA will go 1 → 0
            } else {
                mReadPosA  = newPos;
                mFadingToB = false;  // weightA will go 0 → 1
            }
            mIsXfading     = true;
            mXfadeProgress = 0.0;
        }
    }

    /// 4-point cubic Hermite interpolation at fractional position.
    double hermiteRead(double pos) const {
        int i0 = static_cast<int>(pos);
        double frac = pos - i0;

        int im1 = i0 - 1; if (im1 < 0)            im1 += mBufferSize;
        int ip1 = i0 + 1; if (ip1 >= mBufferSize) ip1 -= mBufferSize;
        int ip2 = i0 + 2; if (ip2 >= mBufferSize) ip2 -= mBufferSize;

        const double y_1 = mBuffer[im1];
        const double y0  = mBuffer[i0];
        const double y1  = mBuffer[ip1];
        const double y2  = mBuffer[ip2];

        const double c0 = y0;
        const double c1 = 0.5 * (y1 - y_1);
        const double c2 = y_1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c3 = 0.5 * (y2 - y_1) + 1.5 * (y0 - y1);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    /// Delay (in samples) from `readPos` back to the write head,
    /// accounting for ring-buffer wrap-around.
    double readDelay(double readPos) const {
        double d = static_cast<double>(mWritePos) - readPos;
        if (d < 0.0) d += static_cast<double>(mBufferSize);
        return d;
    }

    double* mBuffer     = nullptr;
    int32_t mBufferSize = 0;
    int32_t mWritePos   = 0;
    double  mSampleRate = 48000.0;
    double  mPitchRatio = 1.0;
    double  mLastSemitones = 0.0;

    // Two read taps, each a fractional sample index into mBuffer.
    double  mReadPosA = 0.0;
    double  mReadPosB = 0.0;
    // Weight for tap A; tap B's weight is (1 - mWeightA).
    double  mWeightA  = 1.0;

    // Equal-power crossfade state.
    bool    mIsXfading     = false;
    bool    mFadingToB     = false;  // true = fading A→B, false = B→A
    double  mXfadeProgress = 0.0;
    double  mXfadeInc      = 1.0 / 720.0;

    // Drift management.
    double  mTargetDelay   = 1440.0;  // samples; ≈ reported AU latency
    double  mMinDelay      = 288.0;
    double  mMaxDelay      = 8640.0;
};
