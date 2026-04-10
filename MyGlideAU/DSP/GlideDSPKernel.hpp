#pragma once
#include "GlideProcessor.hpp"
#include <AudioToolbox/AudioToolbox.h>

/// Thin wrapper: adapts AudioBufferList to GlideProcessor's float** interface
/// and forwards MIDI / transport state.
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

    void handleMIDIEvent(uint8_t status, uint8_t data1, uint8_t data2) {
        mProcessor.handleMIDIEvent(status, data1, data2);
    }

    void setBeatPosition(double beatPosition, double tempo) {
        mProcessor.setBeatPosition(beatPosition, tempo);
    }

    void* automationCurvePtr() {
        return mProcessor.automationCurvePtr();
    }

    uint64_t activeNoteBitmaskLo() const { return mProcessor.activeNoteBitmaskLo(); }
    uint64_t activeNoteBitmaskHi() const { return mProcessor.activeNoteBitmaskHi(); }
    double currentBeatPosition() const { return mProcessor.currentBeatPosition(); }

private:
    GlideProcessor mProcessor;
    int32_t mChannelCount = 2;
};
