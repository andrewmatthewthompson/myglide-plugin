#pragma once
//
// RythemDSPKernel — thin wrapper around RythemProcessor that exposes an
// Apple-friendly interface (AUParameterAddress / AUValue / UInt32 frame
// counts). Mirrors the role of GlideDSPKernel in MyGlide.
//

#include "RythemProcessor.hpp"
#include <AudioToolbox/AudioToolbox.h>

class RythemDSPKernel {
public:
    void setUp(int32_t channelCount, double sampleRate) {
        mProcessor.setUp(channelCount, sampleRate);
    }

    void tearDown() {
        mProcessor.tearDown();
    }

    void setParameter(AUParameterAddress address, AUValue value) {
        mProcessor.setParameter(static_cast<uint64_t>(address), static_cast<float>(value));
    }

    AUValue getParameter(AUParameterAddress address) const {
        return mProcessor.getParameter(static_cast<uint64_t>(address));
    }

    void setCustomPatternMask(uint32_t mask) { mProcessor.setCustomPatternMask(mask); }
    uint32_t customPatternMask() const       { return mProcessor.customPatternMask(); }

    void handleMIDIEvent(uint8_t status, uint8_t data1, uint8_t data2, uint32_t frameOffset = 0) {
        mProcessor.handleMIDIEvent(status, data1, data2, frameOffset);
    }

    void setBeatPosition(double beatPos, double tempo) {
        mProcessor.setBeatPosition(beatPos, tempo);
    }

    void onTransportJump() { mProcessor.onTransportJump(); }

    void process(AUAudioFrameCount frameCount) {
        mProcessor.process(static_cast<int32_t>(frameCount));
    }

    int pendingMIDIEventCount() const { return mProcessor.pendingCount(); }

    void pendingMIDIEventAt(int i, uint8_t* status, uint8_t* data1, uint8_t* data2, uint32_t* frameOffset) const {
        const auto& e = mProcessor.pendingAt(i);
        if (status)      *status      = e.status;
        if (data1)       *data1       = e.data1;
        if (data2)       *data2       = e.data2;
        if (frameOffset) *frameOffset = e.frameOffset;
    }

    void killAllSounding(uint32_t frameOffset) { mProcessor.killAllSounding(frameOffset); }

    int  heldNoteCount() const     { return mProcessor.heldNoteCount(); }
    int  latchedNoteCount() const  { return mProcessor.latchedNoteCount(); }
    int  currentStepIndex() const  { return mProcessor.currentStepIndex(); }
    bool stepIsActiveNow() const   { return mProcessor.stepIsActiveNow(); }

private:
    myrythem::RythemProcessor mProcessor;
};
