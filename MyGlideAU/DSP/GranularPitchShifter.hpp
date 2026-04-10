#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

/// Real-time granular pitch shifter with 2 overlapping grains.
/// Adapted from MyVerb's shimmer engine with improvements:
///   - Hann window (smoother than triangular) for COLA
///   - Cubic Hermite interpolation on reads (less aliasing)
///   - Continuous semitone control for glide automation
///
/// RT-safe: uses pre-allocated external buffer, no allocations in process().
class GranularPitchShifter {
public:
    GranularPitchShifter() = default;

    /// Configure with external memory. grainMs = grain duration (default 30ms).
    /// bufferPtr must point to at least bufferSize doubles.
    void configure(double* bufferPtr, int32_t bufferSize, double sampleRate, double grainMs = 30.0) {
        mBuffer = bufferPtr;
        mBufferSize = bufferSize;
        mSampleRate = sampleRate;
        mGrainSamples = static_cast<int32_t>(grainMs * 0.001 * sampleRate);
        if (mGrainSamples < 4) mGrainSamples = 4;

        // Clear buffer
        std::memset(mBuffer, 0, sizeof(double) * bufferSize);

        mWritePos = 0;
        mPitchRatio = 1.0;

        // Initialize two grains at 50% phase offset.
        // Initial age = grainSamples * max(pitchRatio, 1) to ensure enough
        // buffered data for the read head to traverse.
        double initAge = static_cast<double>(mGrainSamples) * 2.0;
        mGrains[0].phase = 0.0;
        mGrains[0].age = initAge;
        mGrains[1].phase = 0.5;
        mGrains[1].age = initAge + mGrainSamples * 0.5;
    }

    /// Set pitch shift in semitones. 0 = no shift, +12 = octave up, -12 = octave down.
    void setPitchSemitones(double semitones) {
        mPitchRatio = std::pow(2.0, semitones / 12.0);
    }

    /// Process a single sample. Write input, read pitch-shifted output.
    double process(double input) {
        if (!mBuffer || mBufferSize == 0) return input;

        // Write input to circular buffer
        mBuffer[mWritePos] = input;
        mWritePos = (mWritePos + 1) % mBufferSize;

        double output = 0.0;
        double grainSize = static_cast<double>(mGrainSamples);

        for (int g = 0; g < 2; ++g) {
            Grain& grain = mGrains[g];

            // Hann window based on phase (0..1 within grain)
            double window = 0.5 * (1.0 - std::cos(2.0 * M_PI * grain.phase));

            // Read position: write head minus age
            double readPos = static_cast<double>(mWritePos) - grain.age;
            // Wrap into buffer range
            while (readPos < 0.0) readPos += mBufferSize;

            // Cubic Hermite interpolation
            output += window * hermiteRead(readPos);

            // Advance grain: write head moves +1 per sample, read head moves +pitchRatio.
            // Age (gap between write and read) changes by (1 - pitchRatio).
            // For pitch up (ratio>1), age decreases (read catches up).
            // For pitch down (ratio<1), age increases (read falls behind).
            grain.age += (1.0 - mPitchRatio);
            grain.phase += 1.0 / grainSize;

            // Reset grain when it completes
            if (grain.phase >= 1.0) {
                grain.phase -= 1.0;
                // Reset age: need enough buffered data for the grain to read
                grain.age = grainSize * std::max(mPitchRatio, 1.0);
            }

            // Safety: keep age within valid buffer range
            double maxAge = static_cast<double>(mBufferSize - 4);
            if (grain.age > maxAge) grain.age = grainSize;
            if (grain.age < 2.0) grain.age = grainSize;
        }

        return output;
    }

    /// Get current pitch ratio (for display).
    double pitchRatio() const { return mPitchRatio; }

private:
    struct Grain {
        double phase = 0.0;  // 0..1 within grain
        double age   = 0.0;  // samples behind write head
    };

    /// 4-point cubic Hermite interpolation at fractional position.
    double hermiteRead(double pos) const {
        int i0 = static_cast<int>(pos);
        double frac = pos - i0;

        // 4 adjacent samples
        double y_1 = mBuffer[((i0 - 1) % mBufferSize + mBufferSize) % mBufferSize];
        double y0  = mBuffer[i0 % mBufferSize];
        double y1  = mBuffer[(i0 + 1) % mBufferSize];
        double y2  = mBuffer[(i0 + 2) % mBufferSize];

        // Hermite coefficients
        double c0 = y0;
        double c1 = 0.5 * (y1 - y_1);
        double c2 = y_1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        double c3 = 0.5 * (y2 - y_1) + 1.5 * (y0 - y1);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    double* mBuffer     = nullptr;
    int32_t mBufferSize = 0;
    int32_t mWritePos   = 0;
    double  mSampleRate = 48000.0;
    int32_t mGrainSamples = 1440;  // 30ms @ 48kHz
    double  mPitchRatio = 1.0;

    Grain mGrains[2];
};
