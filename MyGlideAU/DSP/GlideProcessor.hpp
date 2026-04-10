#pragma once
#include "ParameterSmoother.hpp"
#include "AutomationCurve.hpp"
#include "GranularPitchShifter.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>

/// Parameter addresses — keep in sync with GlideParameters.swift
enum ParamAddress : uint64_t {
    kGlideTime  = 0,   // Pitch smoothing rate (ms)
    kMix        = 1,   // Wet/dry mix (%)
    kPitchRange = 2,   // Display range: 12 or 24 semitones
};

/// MIDI pitch automation processor.
///
/// Architecture:
///   - Receives MIDI note on/off → tracks active notes for UI display
///   - Reads beat position from host transport
///   - Evaluates AutomationCurve at current beat → target semitones
///   - Smooths pitch via ParameterSmoother (glide time controls convergence)
///   - Applies pitch shift via GranularPitchShifter
///   - Mixes wet/dry per the mix parameter
///
/// RT-safe: single contiguous memory allocation in setUp(), no heap in process().
class GlideProcessor {
public:
    GlideProcessor() = default;
    ~GlideProcessor() { tearDown(); }

    void setUp(int32_t channelCount, double sampleRate) {
        mChannelCount = channelCount;
        mSampleRate   = sampleRate;

        // Configure parameter smoothers
        mSmGlideTime.configure(sampleRate);
        mSmMix.configure(sampleRate);
        mSmGlideTime.setTarget(50.0);    // 50ms default smoothing
        mSmMix.setTarget(100.0);          // 100% wet default

        // Pitch smoother: convergence time set dynamically from glideTime param
        mSmPitch.configure(sampleRate, 50.0);
        mSmPitch.setTarget(0.0);

        // Allocate single contiguous memory block for pitch shifters
        // Each channel needs a circular buffer for the pitch shifter
        // Buffer size: 0.5 seconds of audio (generous for pitch shifting)
        mPitchBufSize = static_cast<int32_t>(sampleRate * 0.5);
        int totalSamples = mPitchBufSize * channelCount;

        delete[] mMemory;
        mMemory = new double[totalSamples]();

        // Configure pitch shifters with their slice of memory
        for (int32_t ch = 0; ch < channelCount && ch < kMaxChannels; ++ch) {
            double* buf = mMemory + ch * mPitchBufSize;
            mPitchShifters[ch].configure(buf, mPitchBufSize, sampleRate, 30.0);
        }

        // Reset MIDI state
        std::memset(mActiveNotes, 0, sizeof(mActiveNotes));
        mNoteBitmaskLo.store(0, std::memory_order_relaxed);
        mNoteBitmaskHi.store(0, std::memory_order_relaxed);
    }

    void tearDown() {
        delete[] mMemory;
        mMemory = nullptr;
    }

    void setParameter(uint64_t address, float value) {
        switch (static_cast<ParamAddress>(address)) {
            case kGlideTime:  mSmGlideTime.setTarget(value); break;
            case kMix:        mSmMix.setTarget(value);       break;
            case kPitchRange: mPitchRange = static_cast<int>(value); break;
        }
    }

    float getParameter(uint64_t address) const {
        switch (static_cast<ParamAddress>(address)) {
            case kGlideTime:  return static_cast<float>(mSmGlideTime.current());
            case kMix:        return static_cast<float>(mSmMix.current());
            case kPitchRange: return static_cast<float>(mPitchRange);
        }
        return 0.0f;
    }

    // ── MIDI ─────────────────────────────────────────────────────────────

    void handleMIDIEvent(uint8_t status, uint8_t data1, uint8_t data2) {
        uint8_t type = status & 0xF0;
        uint8_t note = data1 & 0x7F;
        uint8_t velocity = data2 & 0x7F;

        if (type == 0x90 && velocity > 0) {
            // Note On
            mActiveNotes[note] = velocity;
        } else if (type == 0x80 || (type == 0x90 && velocity == 0)) {
            // Note Off
            mActiveNotes[note] = 0;
        }

        // Update bitmasks for UI
        uint64_t lo = 0, hi = 0;
        for (int i = 0; i < 64; ++i) {
            if (mActiveNotes[i] > 0) lo |= (1ULL << i);
        }
        for (int i = 64; i < 128; ++i) {
            if (mActiveNotes[i] > 0) hi |= (1ULL << (i - 64));
        }
        mNoteBitmaskLo.store(lo, std::memory_order_relaxed);
        mNoteBitmaskHi.store(hi, std::memory_order_relaxed);
    }

    // ── Transport ────────────────────────────────────────────────────────

    void setBeatPosition(double beatPosition, double tempo) {
        mBeatPosition.store(beatPosition, std::memory_order_relaxed);
        mTempo.store(tempo, std::memory_order_relaxed);
    }

    // ── Process ──────────────────────────────────────────────────────────

    void process(float** buffers, int32_t channelCount, int32_t frameCount) {
        // Swap automation buffer if UI committed changes
        mAutomation.swapIfPending();

        double beatPos = mBeatPosition.load(std::memory_order_relaxed);
        double tempo = mTempo.load(std::memory_order_relaxed);
        double beatsPerSample = (tempo > 0.0) ? (tempo / 60.0 / mSampleRate) : 0.0;

        for (int32_t frame = 0; frame < frameCount; ++frame) {
            const double glideTimeMs = mSmGlideTime.next();
            const double mix = mSmMix.next();
            const double wetGain = mix / 100.0;
            const double dryGain = 1.0 - wetGain;

            // Evaluate automation curve at current beat position
            double targetSemitones = mAutomation.evaluate(beatPos);

            // Update pitch smoother convergence rate from glideTime parameter
            // Reconfigure is cheap — just a coefficient update
            double convergeSamples = glideTimeMs * 0.001 * mSampleRate;
            double coeff = (convergeSamples > 0.0) ? std::exp(-1.0 / convergeSamples) : 0.0;
            mSmPitch.setTarget(targetSemitones);

            double smoothedSemitones = mSmPitch.next();

            // Apply pitch shift to each channel
            for (int32_t ch = 0; ch < channelCount && ch < kMaxChannels; ++ch) {
                double input = static_cast<double>(buffers[ch][frame]) + 1e-20;

                mPitchShifters[ch].setPitchSemitones(smoothedSemitones);
                double wet = mPitchShifters[ch].process(input);

                buffers[ch][frame] = static_cast<float>(input * dryGain + wet * wetGain);
            }

            beatPos += beatsPerSample;
        }

        // Store updated beat position for next block
        mBeatPosition.store(beatPos, std::memory_order_relaxed);
    }

    // ── Accessors for UI ─────────────────────────────────────────────────

    void* automationCurvePtr() { return &mAutomation; }

    uint64_t activeNoteBitmaskLo() const {
        return mNoteBitmaskLo.load(std::memory_order_relaxed);
    }
    uint64_t activeNoteBitmaskHi() const {
        return mNoteBitmaskHi.load(std::memory_order_relaxed);
    }
    double currentBeatPosition() const {
        return mBeatPosition.load(std::memory_order_relaxed);
    }

private:
    static constexpr int kMaxChannels = 2;

    int32_t mChannelCount = 2;
    double  mSampleRate   = 48000.0;
    int     mPitchRange   = 24;

    // Parameter smoothers
    ParameterSmoother mSmGlideTime;
    ParameterSmoother mSmMix;
    ParameterSmoother mSmPitch;   // smooths the automation curve output

    // Pitch shifting
    GranularPitchShifter mPitchShifters[kMaxChannels];
    double* mMemory     = nullptr;
    int32_t mPitchBufSize = 0;

    // Automation curve (triple-buffered, lock-free)
    AutomationCurve mAutomation;

    // MIDI note state
    uint8_t mActiveNotes[128] = {};
    std::atomic<uint64_t> mNoteBitmaskLo{0};
    std::atomic<uint64_t> mNoteBitmaskHi{0};

    // Transport
    std::atomic<double> mBeatPosition{0.0};
    std::atomic<double> mTempo{120.0};
};
