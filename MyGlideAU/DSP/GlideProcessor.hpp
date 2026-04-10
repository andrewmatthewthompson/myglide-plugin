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
///   - Receives MIDI note on/off -> tracks active notes for UI display
///   - Reads beat position from host transport
///   - Evaluates AutomationCurve at current beat -> target semitones
///   - Smooths pitch via ParameterSmoother (glide time controls convergence)
///   - Applies pitch shift via GranularPitchShifter
///   - Mixes wet/dry per the mix parameter
///
/// RT-safe: single contiguous memory allocation in setUp(), no heap in process().
class GlideProcessor {
public:
    GlideProcessor() = default;
    ~GlideProcessor() { tearDown(); }

    // Non-copyable (owns raw memory, contains atomics)
    GlideProcessor(const GlideProcessor&) = delete;
    GlideProcessor& operator=(const GlideProcessor&) = delete;

    void setUp(int32_t channelCount, double sampleRate) {
        // Tear down any existing state first
        tearDown();

        mChannelCount = channelCount;
        mSampleRate   = sampleRate;

        // Configure parameter smoothers
        mSmGlideTime.configure(sampleRate);
        mSmMix.configure(sampleRate);
        mSmGlideTime.setTarget(50.0);    // 50ms default smoothing
        mSmMix.setTarget(100.0);          // 100% wet default

        // Pitch smoother: initial convergence from default glide time
        mSmPitch.configure(sampleRate, 50.0);
        mSmPitch.setTarget(0.0);
        mLastGlideTimeMs = 50.0;

        // Allocate single contiguous memory block for:
        //   - Pitch shifter circular buffers (0.5s per channel)
        //   - Shared Hann window LUT (one copy, both channels use it)
        mPitchBufSize = static_cast<int32_t>(sampleRate * 0.5);
        mGrainSamples = static_cast<int32_t>(0.030 * sampleRate);  // 30ms
        if (mGrainSamples < 4) mGrainSamples = 4;

        int32_t clampedChannels = std::min(channelCount, static_cast<int32_t>(kMaxChannels));
        int totalSamples = mPitchBufSize * clampedChannels + mGrainSamples;

        mMemory = new double[totalSamples]();

        // Layout: [channel0 buffer | channel1 buffer | shared Hann LUT]
        double* hannPtr = mMemory + mPitchBufSize * clampedChannels;

        // Precompute shared Hann window (both channels use the same grain size)
        for (int32_t i = 0; i < mGrainSamples; ++i) {
            double phase = static_cast<double>(i) / static_cast<double>(mGrainSamples);
            hannPtr[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * phase));
        }

        // Configure pitch shifters with their slice of memory + shared Hann LUT
        for (int32_t ch = 0; ch < clampedChannels; ++ch) {
            double* buf = mMemory + ch * mPitchBufSize;
            mPitchShifters[ch].configure(buf, mPitchBufSize, hannPtr, mGrainSamples, sampleRate);
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
            mActiveNotes[note] = velocity;
            // Set bit
            if (note < 64)
                mNoteBitmaskLo.fetch_or(1ULL << note, std::memory_order_relaxed);
            else
                mNoteBitmaskHi.fetch_or(1ULL << (note - 64), std::memory_order_relaxed);
        } else if (type == 0x80 || (type == 0x90 && velocity == 0)) {
            mActiveNotes[note] = 0;
            // Clear bit
            if (note < 64)
                mNoteBitmaskLo.fetch_and(~(1ULL << note), std::memory_order_relaxed);
            else
                mNoteBitmaskHi.fetch_and(~(1ULL << (note - 64)), std::memory_order_relaxed);
        }
    }

    // ── Transport ────────────────────────────────────────────────────────

    void setBeatPosition(double beatPosition, double tempo) {
        mBeatPosition.store(beatPosition, std::memory_order_relaxed);
        mTempo.store(tempo, std::memory_order_relaxed);
    }

    // ── Process ──────────────────────────────────────────────────────────

    void process(float** buffers, int32_t channelCount, int32_t frameCount) {
        if (!mMemory) return;

        // Swap automation buffer if UI committed changes
        mAutomation.swapIfPending();

        double beatPos = mBeatPosition.load(std::memory_order_relaxed);
        double tempo = mTempo.load(std::memory_order_relaxed);
        double beatsPerSample = (tempo > 0.0) ? (tempo / 60.0 / mSampleRate) : 0.0;
        int32_t clampedChannels = std::min(channelCount, static_cast<int32_t>(kMaxChannels));

        for (int32_t frame = 0; frame < frameCount; ++frame) {
            const double glideTimeMs = mSmGlideTime.next();
            const double mix = mSmMix.next();
            const double wetGain = mix / 100.0;
            const double dryGain = 1.0 - wetGain;

            // Update pitch smoother rate when glide time changes significantly.
            // Uses updateCoefficient (not configure) to avoid resetting mCurrent,
            // which would cause audible pitch discontinuities.
            if (std::fabs(glideTimeMs - mLastGlideTimeMs) > 0.5) {
                mSmPitch.updateCoefficient(mSampleRate, glideTimeMs);
                mLastGlideTimeMs = glideTimeMs;
            }

            // Evaluate automation curve at current beat position
            double targetSemitones = mAutomation.evaluate(beatPos);
            mSmPitch.setTarget(targetSemitones);
            double smoothedSemitones = mSmPitch.next();

            // Apply pitch shift to each channel
            for (int32_t ch = 0; ch < clampedChannels; ++ch) {
                double input = static_cast<double>(buffers[ch][frame]) + 1e-20;

                mPitchShifters[ch].setPitchSemitones(smoothedSemitones);
                double wet = mPitchShifters[ch].process(input);

                buffers[ch][frame] = static_cast<float>(input * dryGain + wet * wetGain);
            }

            beatPos += beatsPerSample;
        }

        // Store updated beat position and display pitch for next block / UI
        mBeatPosition.store(beatPos, std::memory_order_relaxed);
        mDisplayPitch.store(mSmPitch.current(), std::memory_order_relaxed);
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

    /// Latency in samples introduced by the granular pitch shifter.
    /// Logic Pro uses this for Plugin Delay Compensation (PDC).
    int32_t latencySamples() const { return mGrainSamples; }

    /// Tail time in seconds: how long audio rings out after input stops.
    double tailTimeSeconds() const {
        return (mSampleRate > 0.0) ? (double(mGrainSamples) / mSampleRate) : 0.0;
    }

    /// Current smoothed pitch shift in semitones (for UI display).
    double currentPitchSemitones() const {
        return mDisplayPitch.load(std::memory_order_relaxed);
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
    double mLastGlideTimeMs = 50.0; // cache to avoid reconfiguring every sample

    // Pitch shifting
    GranularPitchShifter mPitchShifters[kMaxChannels];
    double* mMemory      = nullptr;
    int32_t mPitchBufSize = 0;
    int32_t mGrainSamples = 1440;

    // Automation curve (triple-buffered, lock-free)
    AutomationCurve mAutomation;

    // MIDI note state
    uint8_t mActiveNotes[128] = {};
    std::atomic<uint64_t> mNoteBitmaskLo{0};
    std::atomic<uint64_t> mNoteBitmaskHi{0};

    // Transport
    std::atomic<double> mBeatPosition{0.0};
    std::atomic<double> mTempo{120.0};

    // Display (written once per block by audio thread, read by UI timer)
    std::atomic<double> mDisplayPitch{0.0};
};
