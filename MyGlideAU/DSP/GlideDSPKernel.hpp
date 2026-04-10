#pragma once
#include "GlideProcessor.hpp"
#include <AudioToolbox/AudioToolbox.h>

/// Thin wrapper: adapts AudioBufferList to GlideProcessor's float** interface.
class GlideDSPKernel {
public:
    void setUp(int32_t channelCount, double sampleRate) {
        mProcessor.setUp(channelCount, sampleRate);
        mChannelCount = channelCount;
    }

    void tearDown() {
        mProcessor.tearDown();
    }

    void setParameter(AUParameterAddress address, AUValue value) {
        mProcessor.setParameter(address, value);
    }

    AUValue getParameter(AUParameterAddress address) const {
        return mProcessor.getParameter(address);
    }

    void process(AudioBufferList* bufferList, AUAudioFrameCount frameCount) {
        float* channels[2] = { nullptr, nullptr };
        for (int32_t i = 0; i < mChannelCount && i < 2; ++i) {
            channels[i] = static_cast<float*>(bufferList->mBuffers[i].mData);
        }
        mProcessor.process(channels, mChannelCount, static_cast<int32_t>(frameCount));
    }

private:
    GlideProcessor mProcessor;
    int32_t mChannelCount = 2;
};
