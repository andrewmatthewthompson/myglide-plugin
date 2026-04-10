#pragma once
#include "ParameterSmoother.hpp"
#include <cstdint>
#include <cstring>

/// Parameter addresses — keep in sync with GlideParameters.swift
enum ParamAddress : uint64_t {
    kGlideTime = 0,
    kMix       = 1,
};

/// Placeholder glide/portamento DSP processor.
/// Architecture follows MyVerb conventions:
///   - Double-precision internal processing
///   - Single contiguous memory allocation in setUp()
///   - No dynamic allocation in process()
///   - Parameter smoothing on all automatable params
///   - Denormal guarding
class GlideProcessor {
public:
    GlideProcessor() = default;
    ~GlideProcessor() { tearDown(); }

    void setUp(int32_t channelCount, double sampleRate) {
        mChannelCount = channelCount;
        mSampleRate   = sampleRate;

        mSmGlideTime.configure(sampleRate);
        mSmMix.configure(sampleRate);

        mSmGlideTime.setTarget(200.0);   // 200 ms default
        mSmMix.setTarget(100.0);         // 100% wet default

        // TODO: Allocate DSP memory here (single contiguous block)
    }

    void tearDown() {
        // TODO: Free DSP memory here
    }

    void setParameter(uint64_t address, float value) {
        switch (static_cast<ParamAddress>(address)) {
            case kGlideTime: mSmGlideTime.setTarget(value); break;
            case kMix:       mSmMix.setTarget(value);       break;
        }
    }

    float getParameter(uint64_t address) const {
        switch (static_cast<ParamAddress>(address)) {
            case kGlideTime: return static_cast<float>(mSmGlideTime.current());
            case kMix:       return static_cast<float>(mSmMix.current());
        }
        return 0.0f;
    }

    void process(float** buffers, int32_t channelCount, int32_t frameCount) {
        for (int32_t frame = 0; frame < frameCount; ++frame) {
            const double glideTime = mSmGlideTime.next();
            const double mix       = mSmMix.next();
            const double wetGain   = mix / 100.0;
            const double dryGain   = 1.0 - wetGain;

            // Denormal guard
            (void)glideTime;

            for (int32_t ch = 0; ch < channelCount; ++ch) {
                double input = static_cast<double>(buffers[ch][frame]) + 1e-20;

                // TODO: Implement glide/portamento DSP here
                double wet = input;  // passthrough for now

                buffers[ch][frame] = static_cast<float>(input * dryGain + wet * wetGain);
            }
        }
    }

private:
    int32_t mChannelCount = 2;
    double  mSampleRate   = 48000.0;

    ParameterSmoother mSmGlideTime;
    ParameterSmoother mSmMix;
};
