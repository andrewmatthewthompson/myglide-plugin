#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

/// Real-time granular pitch shifter with 2 overlapping grains.
/// Adapted from MyVerb's shimmer engine with improvements:
///   - Hann window via precomputed LUT (no trig per sample)
///   - Cubic Hermite interpolation on reads (less aliasing)
///   - Continuous semitone control for glide automation
///   - Cached pitch ratio to avoid pow() per-sample
///
/// Memory model: all buffers (circular + Hann LUT) are external pointers into
/// a contiguous block owned by the parent GlideProcessor. The class itself is
/// only ~96 bytes — no large inline arrays.
///
/// RT-safe: no allocations in process().
class GranularPitchShifter {
public:
    GranularPitchShifter() = default;

    // Non-copyable: holds non-owning pointers to shared memory.
    GranularPitchShifter(const GranularPitchShifter&) = delete;
    GranularPitchShifter& operator=(const GranularPitchShifter&) = delete;

    /// Configure with external memory for both the circular buffer and Hann LUT.
    /// bufferPtr: circular buffer (bufferSize doubles)
    /// hannPtr: precomputed Hann window (grainSamples doubles), may be shared across channels
    void configure(double* bufferPtr, int32_t bufferSize,
                   double* hannPtr, int32_t grainSamples,
                   double sampleRate) {
        mBuffer = bufferPtr;
        mBufferSize = bufferSize;
        mHannLUT = hannPtr;
        mGrainSamples = grainSamples;
        mSampleRate = sampleRate;

        std::memset(mBuffer, 0, sizeof(double) * bufferSize);

        mWritePos = 0;
        mPitchRatio = 1.0;
        mLastSemitones = 0.0;

        // Two grains at 50% phase offset but starting at the SAME age.
        // Hann windows at 50% overlap sum to exactly 1, so as long as
        // the grains read from the same delay position the output at
        // unity pitch is a clean `mGrainSamples`-sample delayed copy
        // of the input — which is what the `latencySamples()` value
        // promises the host. Using different initial ages (as before)
        // made the grains read from different delays, so their sum
        // comb-filtered the signal even at ratio 1.
        const double restingAge = static_cast<double>(mGrainSamples);
        mGrains[0].phase = 0.0;
        mGrains[0].sampleIndex = 0;
        mGrains[0].age = restingAge;
        mGrains[1].phase = 0.5;
        mGrains[1].sampleIndex = mGrainSamples / 2;
        mGrains[1].age = restingAge;
    }

    /// Write a single input sample into the ring buffer without performing
    /// grain reads. Used by GlideProcessor during the block-level bypass
    /// path so the buffer stays fresh and the shifter can resume without
    /// a stale-audio pop when pitch shifting re-engages.
    void writeInputOnly(double input) {
        if (!mBuffer || mBufferSize == 0) return;
        mBuffer[mWritePos] = input;
        mWritePos = (mWritePos + 1) % mBufferSize;
    }

    /// Set pitch shift in semitones. 0 = no shift, +12 = octave up, -12 = octave down.
    /// Clamped to ±48 semitones (4 octaves) to prevent buffer overrun from extreme ratios.
    /// Only recalculates pow() when the value actually changes (>0.001 threshold).
    void setPitchSemitones(double semitones) {
        if (semitones > 48.0) semitones = 48.0;
        if (semitones < -48.0) semitones = -48.0;

        if (std::fabs(semitones - mLastSemitones) > 0.001) {
            mPitchRatio = std::pow(2.0, semitones / 12.0);
            mLastSemitones = semitones;
        }
    }

    /// Process a single sample. Write input, read pitch-shifted output.
    double process(double input) {
        if (!mBuffer || mBufferSize == 0) return input;

        // Write input to circular buffer
        mBuffer[mWritePos] = input;
        mWritePos = (mWritePos + 1) % mBufferSize;

        double output = 0.0;
        const double grainSize = static_cast<double>(mGrainSamples);
        const double bufSize = static_cast<double>(mBufferSize);

        for (int g = 0; g < 2; ++g) {
            Grain& grain = mGrains[g];

            // Hann window from precomputed LUT (no trig)
            int32_t lutIdx = grain.sampleIndex;
            if (lutIdx < 0) lutIdx = 0;
            if (lutIdx >= mGrainSamples) lutIdx = mGrainSamples - 1;
            double window = mHannLUT[lutIdx];

            // Read position: write head minus age, wrapped into [0, bufSize).
            // With ±48 semitone clamp (ratio ≤16), max age after reset = grainSize*16.
            // Single conditional wrap is safe: |readPos| ≤ grainSize*16 < bufSize
            // (bufSize = 0.5s of audio; grainSize*16 = 30ms*16 = 480ms < 500ms).
            double readPos = static_cast<double>(mWritePos) - grain.age;
            if (readPos < 0.0) readPos += bufSize;
            else if (readPos >= bufSize) readPos -= bufSize;

            // Cubic Hermite interpolation
            output += window * hermiteRead(readPos);

            // Advance grain
            grain.age += (1.0 - mPitchRatio);
            grain.phase += 1.0 / grainSize;
            grain.sampleIndex++;

            // Reset grain when it completes
            if (grain.phase >= 1.0) {
                grain.phase -= 1.0;
                grain.sampleIndex = 0;
                grain.age = grainSize * std::max(mPitchRatio, 1.0);
            }

            // Safety: keep age within valid buffer range
            double maxAge = bufSize - 4.0;
            if (grain.age > maxAge || grain.age < 2.0 ||
                std::isnan(grain.age) || std::isinf(grain.age)) {
                grain.age = grainSize;
            }
        }

        return output;
    }

    /// Get current pitch ratio (for display).
    double pitchRatio() const { return mPitchRatio; }

private:
    struct Grain {
        double phase = 0.0;
        double age   = 0.0;
        int32_t sampleIndex = 0;
    };

    /// 4-point cubic Hermite interpolation at fractional position.
    double hermiteRead(double pos) const {
        int i0 = static_cast<int>(pos);
        double frac = pos - i0;

        int im1 = i0 - 1;
        if (im1 < 0) im1 += mBufferSize;
        int ip1 = i0 + 1;
        if (ip1 >= mBufferSize) ip1 -= mBufferSize;
        int ip2 = i0 + 2;
        if (ip2 >= mBufferSize) ip2 -= mBufferSize;

        double y_1 = mBuffer[im1];
        double y0  = mBuffer[i0];
        double y1  = mBuffer[ip1];
        double y2  = mBuffer[ip2];

        double c0 = y0;
        double c1 = 0.5 * (y1 - y_1);
        double c2 = y_1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        double c3 = 0.5 * (y2 - y_1) + 1.5 * (y0 - y1);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    double* mBuffer     = nullptr;
    int32_t mBufferSize = 0;
    int32_t mWritePos   = 0;
    double* mHannLUT    = nullptr;   // external: shared Hann window
    int32_t mGrainSamples = 1440;
    double  mSampleRate = 48000.0;
    double  mPitchRatio = 1.0;
    double  mLastSemitones = 0.0;

    Grain   mGrains[2];
};
