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
/// RT-safe: uses pre-allocated external buffer, no allocations in process().
class GranularPitchShifter {
public:
    GranularPitchShifter() = default;

    // Non-copyable: holds a non-owning pointer to shared buffer memory.
    GranularPitchShifter(const GranularPitchShifter&) = delete;
    GranularPitchShifter& operator=(const GranularPitchShifter&) = delete;

    /// Configure with external memory. grainMs = grain duration (default 30ms).
    /// bufferPtr must point to at least bufferSize doubles.
    void configure(double* bufferPtr, int32_t bufferSize, double sampleRate, double grainMs = 30.0) {
        mBuffer = bufferPtr;
        mBufferSize = bufferSize;
        mSampleRate = sampleRate;
        mGrainSamples = static_cast<int32_t>(grainMs * 0.001 * sampleRate);
        if (mGrainSamples < 4) mGrainSamples = 4;
        if (mGrainSamples > kMaxGrainSamples) mGrainSamples = kMaxGrainSamples;

        // Precompute Hann window LUT (eliminates cos() from the audio loop)
        for (int32_t i = 0; i < mGrainSamples; ++i) {
            double phase = static_cast<double>(i) / static_cast<double>(mGrainSamples);
            mHannLUT[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * phase));
        }

        std::memset(mBuffer, 0, sizeof(double) * bufferSize);

        mWritePos = 0;
        mPitchRatio = 1.0;
        mLastSemitones = 0.0;

        // Initialize two grains at 50% phase offset.
        double initAge = static_cast<double>(mGrainSamples) * 2.0;
        mGrains[0].phase = 0.0;
        mGrains[0].sampleIndex = 0;
        mGrains[0].age = initAge;
        mGrains[1].phase = 0.5;
        mGrains[1].sampleIndex = mGrainSamples / 2;
        mGrains[1].age = initAge + mGrainSamples * 0.5;
    }

    /// Set pitch shift in semitones. 0 = no shift, +12 = octave up, -12 = octave down.
    /// Only recalculates pow() when the value actually changes (>0.001 threshold).
    void setPitchSemitones(double semitones) {
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

            // Read position: write head minus age
            double readPos = static_cast<double>(mWritePos) - grain.age;
            // Conditional wrap (cheaper than fmod — readPos is in [-bufSize, +bufSize] range)
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
        double phase = 0.0;       // 0..1 within grain
        double age   = 0.0;       // samples behind write head
        int32_t sampleIndex = 0;  // integer index into Hann LUT
    };

    /// 4-point cubic Hermite interpolation at fractional position.
    double hermiteRead(double pos) const {
        int i0 = static_cast<int>(pos);
        double frac = pos - i0;

        // Wrap indices safely — pos is already in [0, bufferSize) so i0 is valid,
        // but i0-1 and i0+2 need wrapping.
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

        // Hermite coefficients
        double c0 = y0;
        double c1 = 0.5 * (y1 - y_1);
        double c2 = y_1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        double c3 = 0.5 * (y2 - y_1) + 1.5 * (y0 - y1);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    // Max grain size: 100ms @ 192kHz = 19200 samples
    static constexpr int32_t kMaxGrainSamples = 19200;

    double* mBuffer     = nullptr;
    int32_t mBufferSize = 0;
    int32_t mWritePos   = 0;
    double  mSampleRate = 48000.0;
    int32_t mGrainSamples = 1440;
    double  mPitchRatio = 1.0;
    double  mLastSemitones = 0.0;

    double  mHannLUT[kMaxGrainSamples] = {};  // precomputed Hann window
    Grain   mGrains[2];
};
