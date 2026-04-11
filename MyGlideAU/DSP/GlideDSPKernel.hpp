#pragma once
#include "GlideProcessor.hpp"
#include <AudioToolbox/AudioToolbox.h>

/// Thin wrapper: adapts AudioBufferList to GlideProcessor's float** interface
/// and forwards MIDI / transport / automation state.
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

    // ── Automation curve bridge (called from UI thread via Obj-C) ────────

    void automationBeginEdit() {
        auto* curve = mProcessor.automationCurvePtr();
        curve->beginEdit();
    }

    void automationAddBreakpoint(double beat, double semitones, uint8_t interpType) {
        auto* curve = mProcessor.automationCurvePtr();
        curve->addBreakpoint(beat, semitones, static_cast<InterpolationType>(interpType));
    }

    void automationRemoveBreakpoint(int index) {
        auto* curve = mProcessor.automationCurvePtr();
        curve->removeBreakpoint(index);
    }

    void automationMoveBreakpoint(int index, double beat, double semitones) {
        auto* curve = mProcessor.automationCurvePtr();
        curve->moveBreakpoint(index, beat, semitones);
    }

    void automationClear() {
        auto* curve = mProcessor.automationCurvePtr();
        // No beginEdit() here — caller must have already called automationBeginEdit().
        // Calling beginEdit() again would pick a potentially different write buffer,
        // discarding any work done since the first beginEdit().
        curve->clearBreakpoints();
    }

    void automationCommitEdit() {
        auto* curve = mProcessor.automationCurvePtr();
        curve->commitEdit();
    }

    uint64_t activeNoteBitmaskLo() const { return mProcessor.activeNoteBitmaskLo(); }
    uint64_t activeNoteBitmaskHi() const { return mProcessor.activeNoteBitmaskHi(); }
    double currentBeatPosition() const { return mProcessor.currentBeatPosition(); }
    double currentPitchSemitones() const { return mProcessor.currentPitchSemitones(); }

    int32_t latencySamples() const { return mProcessor.latencySamples(); }
    double tailTimeSeconds() const { return mProcessor.tailTimeSeconds(); }

    double inputLevelL() const { return mProcessor.inputLevelL(); }
    double inputLevelR() const { return mProcessor.inputLevelR(); }
    double outputLevelL() const { return mProcessor.outputLevelL(); }
    double outputLevelR() const { return mProcessor.outputLevelR(); }
    double autoGlideTarget() const { return mProcessor.autoGlideTarget(); }

    // ── Serialization (for preset save/load) ─────────────────────────────

    int automationSerialize(uint8_t* out, int maxBytes) {
        auto* curve = mProcessor.automationCurvePtr();
        return curve->serialize(out, maxBytes);
    }

    void automationDeserialize(const uint8_t* data, int length) {
        auto* curve = mProcessor.automationCurvePtr();
        curve->beginEdit();
        curve->deserialize(data, length);
        curve->commitEdit();
    }

    int automationBreakpointCount() {
        auto* curve = mProcessor.automationCurvePtr();
        return curve->count();
    }

    void automationBreakpointAt(int index, double* beat, double* semitones, uint8_t* interpType) {
        auto* curve = mProcessor.automationCurvePtr();
        const Breakpoint* pts = nullptr;
        int count = curve->getBreakpoints(&pts);
        if (index >= 0 && index < count && pts) {
            *beat = pts[index].beat;
            *semitones = pts[index].semitones;
            *interpType = static_cast<uint8_t>(pts[index].interp);
        }
    }

private:
    GlideProcessor mProcessor;
    int32_t mChannelCount = 2;
};
