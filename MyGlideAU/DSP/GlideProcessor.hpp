#pragma once
#include "ParameterSmoother.hpp"
#include "AutomationCurve.hpp"
#include "GranularPitchShifter.hpp"
#include "PhaseVocoderPitchShifter.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>

/// Parameter addresses — keep in sync with GlideParameters.swift
enum ParamAddress : uint64_t {
    kGlideTime    = 0,   // Pitch smoothing rate (ms)
    kMix          = 1,   // Wet/dry mix (%)
    kPitchRange   = 2,   // Display range: 12 or 24 semitones
    kPitchOffset  = 3,   // DAW-automatable pitch offset (semitones, ±24)
    kShifterMode  = 4,   // 0=Granular, 1=Vocoder
    kAutoGlide    = 5,   // 0=Manual (automation curve), 1=Auto (MIDI-driven)
};

/// MIDI pitch automation processor.
///
/// Architecture:
///   - Receives MIDI note on/off -> tracks active notes for UI display
///   - Reads beat position from host transport
///   - Evaluates AutomationCurve at current beat -> target semitones
///   - Smooths pitch via ParameterSmoother (glide time controls convergence)
///   - Applies pitch shift via GranularPitchShifter or PhaseVocoderPitchShifter
///   - Mixes wet/dry per the mix parameter
///
/// RT-safe: contiguous memory allocations in setUp(), no heap in process().
class GlideProcessor {
public:
    GlideProcessor() = default;
    ~GlideProcessor() { tearDown(); }

    // Non-copyable (owns raw memory, contains atomics)
    GlideProcessor(const GlideProcessor&) = delete;
    GlideProcessor& operator=(const GlideProcessor&) = delete;

    void setUp(int32_t channelCount, double sampleRate) {
        tearDown();

        mChannelCount = channelCount;
        mSampleRate   = sampleRate;

        // Configure parameter smoothers
        mSmGlideTime.configure(sampleRate);
        mSmMix.configure(sampleRate);
        mSmPitchOffset.configure(sampleRate);
        mSmGlideTime.setTarget(50.0);
        mSmMix.setTarget(100.0);
        mSmPitchOffset.setTarget(0.0);

        mSmPitch.configure(sampleRate, 50.0);
        mSmPitch.setTarget(0.0);
        mLastGlideTimeMs = 50.0;

        mAAFilterL = 0.0;
        mAAFilterR = 0.0;

        int32_t clampedChannels = std::min(channelCount, static_cast<int32_t>(kMaxChannels));

        // ── Granular shifter memory ──────────────────────────────────────
        mPitchBufSize = static_cast<int32_t>(sampleRate * 0.5);
        mGrainSamples = static_cast<int32_t>(0.030 * sampleRate);
        if (mGrainSamples < 4) mGrainSamples = 4;

        int totalGranular = mPitchBufSize * clampedChannels + mGrainSamples;
        mGranularMemory = new double[totalGranular]();

        double* hannGranular = mGranularMemory + mPitchBufSize * clampedChannels;
        for (int32_t i = 0; i < mGrainSamples; ++i) {
            double phase = static_cast<double>(i) / static_cast<double>(mGrainSamples);
            hannGranular[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * phase));
        }

        for (int32_t ch = 0; ch < clampedChannels; ++ch) {
            double* buf = mGranularMemory + ch * mPitchBufSize;
            mGranularShifters[ch].configure(buf, mPitchBufSize, hannGranular, mGrainSamples, sampleRate);
        }

        // ── Phase vocoder memory ─────────────────────────────────────────
        mFFTSize = PhaseVocoderPitchShifter::kFFTSize;
        int32_t hannSizeVocoder = mFFTSize;
        int32_t perChannel = PhaseVocoderPitchShifter::kMemoryPerChannel;
        int totalVocoder = perChannel * clampedChannels + hannSizeVocoder;
        mVocoderMemory = new double[totalVocoder]();

        double* hannVocoder = mVocoderMemory + perChannel * clampedChannels;
        for (int32_t i = 0; i < hannSizeVocoder; ++i) {
            double phase = static_cast<double>(i) / static_cast<double>(hannSizeVocoder);
            hannVocoder[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * phase));
        }

        for (int32_t ch = 0; ch < clampedChannels; ++ch) {
            double* buf = mVocoderMemory + ch * perChannel;
            mVocoderShifters[ch].configure(buf, perChannel, hannVocoder, mFFTSize, sampleRate);
        }

        // Reset MIDI state
        std::memset(mActiveNotes, 0, sizeof(mActiveNotes));
        mNoteBitmaskLo.store(0, std::memory_order_relaxed);
        mNoteBitmaskHi.store(0, std::memory_order_relaxed);
    }

    void tearDown() {
        delete[] mGranularMemory;
        mGranularMemory = nullptr;
        delete[] mVocoderMemory;
        mVocoderMemory = nullptr;
    }

    void setParameter(uint64_t address, float value) {
        switch (static_cast<ParamAddress>(address)) {
            case kGlideTime:    mSmGlideTime.setTarget(value);    break;
            case kMix:          mSmMix.setTarget(value);          break;
            case kPitchRange:   mPitchRange = static_cast<int>(value); break;
            case kPitchOffset:  mSmPitchOffset.setTarget(value);  break;
            case kShifterMode:  mShifterMode = static_cast<int>(value); break;
            case kAutoGlide:    mAutoGlide = static_cast<int>(value);  break;
        }
    }

    float getParameter(uint64_t address) const {
        switch (static_cast<ParamAddress>(address)) {
            case kGlideTime:    return static_cast<float>(mSmGlideTime.current());
            case kMix:          return static_cast<float>(mSmMix.current());
            case kPitchRange:   return static_cast<float>(mPitchRange);
            case kPitchOffset:  return static_cast<float>(mSmPitchOffset.current());
            case kShifterMode:  return static_cast<float>(mShifterMode);
            case kAutoGlide:    return static_cast<float>(mAutoGlide);
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
            if (note < 64)
                mNoteBitmaskLo.fetch_or(1ULL << note, std::memory_order_relaxed);
            else
                mNoteBitmaskHi.fetch_or(1ULL << (note - 64), std::memory_order_relaxed);

            // Auto-glide: new note sets pitch target directly (last-note priority)
            if (mAutoGlide) {
                mLastNoteOn = note;
                double semi = static_cast<double>(note) - static_cast<double>(kReferenceNote);
                mAutoGlideTarget.store(semi, std::memory_order_relaxed);
            }
        } else if (type == 0x80 || (type == 0x90 && velocity == 0)) {
            // Note Off
            mActiveNotes[note] = 0;
            if (note < 64)
                mNoteBitmaskLo.fetch_and(~(1ULL << note), std::memory_order_relaxed);
            else
                mNoteBitmaskHi.fetch_and(~(1ULL << (note - 64)), std::memory_order_relaxed);

            // Auto-glide: if released note was the active one, fall back to highest held note
            if (mAutoGlide && note == mLastNoteOn) {
                uint8_t next = findHighestActiveNote();
                if (next > 0) {
                    mLastNoteOn = next;
                    double semi = static_cast<double>(next) - static_cast<double>(kReferenceNote);
                    mAutoGlideTarget.store(semi, std::memory_order_relaxed);
                }
                // If no notes held, keep last pitch (sustain behavior)
            }
        }
    }

    // ── Transport ────────────────────────────────────────────────────────

    void setBeatPosition(double beatPosition, double tempo) {
        mBeatPosition.store(beatPosition, std::memory_order_relaxed);
        mTempo.store(tempo, std::memory_order_relaxed);
    }

    // ── Process ──────────────────────────────────────────────────────────

    void process(float** buffers, int32_t channelCount, int32_t frameCount) {
        if (!mGranularMemory || !buffers || frameCount <= 0) return;
        if (!buffers[0]) return;

        mAutomation.swapIfPending();

        double beatPos = mBeatPosition.load(std::memory_order_relaxed);
        double tempo = std::max(0.0, mTempo.load(std::memory_order_relaxed));
        double beatsPerSample = (tempo > 0.0) ? (tempo / 60.0 / mSampleRate) : 0.0;
        int32_t clampedChannels = std::min(channelCount, static_cast<int32_t>(kMaxChannels));
        const bool useVocoder = (mShifterMode == 1) && mVocoderMemory;

        // Capture input peak before processing
        {
            double pkL = 0.0, pkR = 0.0;
            for (int32_t i = 0; i < frameCount; ++i) {
                double a = std::fabs(static_cast<double>(buffers[0][i]));
                if (a > pkL) pkL = a;
            }
            if (clampedChannels >= 2) {
                for (int32_t i = 0; i < frameCount; ++i) {
                    double a = std::fabs(static_cast<double>(buffers[1][i]));
                    if (a > pkR) pkR = a;
                }
            } else { pkR = pkL; }
            mInputLevelL.store(pkL, std::memory_order_relaxed);
            mInputLevelR.store(pkR, std::memory_order_relaxed);
        }

        // Anti-aliasing filter coefficient: computed once per block (not per sample).
        // Updated at the end of the block based on final smoothed semitones.
        double aaCoeff = mCachedAACoeff;

        for (int32_t frame = 0; frame < frameCount; ++frame) {
            const double glideTimeMs = mSmGlideTime.next();
            const double mix = mSmMix.next();
            const double pitchOffset = mSmPitchOffset.next();
            const double wetGain = mix / 100.0;
            const double dryGain = 1.0 - wetGain;

            if (std::fabs(glideTimeMs - mLastGlideTimeMs) > 0.5) {
                mSmPitch.updateCoefficient(mSampleRate, glideTimeMs);
                mLastGlideTimeMs = glideTimeMs;
            }

            // In Auto mode, MIDI drives pitch; in Manual mode, automation curve drives pitch
            double targetSemitones;
            if (mAutoGlide) {
                targetSemitones = mAutoGlideTarget.load(std::memory_order_relaxed) + pitchOffset;
            } else {
                targetSemitones = mAutomation.evaluate(beatPos) + pitchOffset;
            }
            mSmPitch.setTarget(targetSemitones);
            double smoothedSemitones = mSmPitch.next();

            for (int32_t ch = 0; ch < clampedChannels; ++ch) {
                double input = static_cast<double>(buffers[ch][frame]) + 1e-20;

                double wet;
                if (useVocoder) {
                    mVocoderShifters[ch].setPitchSemitones(smoothedSemitones);
                    wet = mVocoderShifters[ch].process(input);
                } else {
                    mGranularShifters[ch].setPitchSemitones(smoothedSemitones);
                    wet = mGranularShifters[ch].process(input);
                }

                if (aaCoeff > 0.0) {
                    double& state = (ch == 0) ? mAAFilterL : mAAFilterR;
                    state = state * aaCoeff + wet * (1.0 - aaCoeff);
                    wet = state;
                }

                buffers[ch][frame] = static_cast<float>(input * dryGain + wet * wetGain);
            }

            beatPos += beatsPerSample;
        }

        mBeatPosition.store(beatPos, std::memory_order_relaxed);
        mDisplayPitch.store(mSmPitch.current(), std::memory_order_relaxed);

        // Update cached AA coefficient for next block (avoids pow() per sample)
        {
            double semi = std::fabs(mSmPitch.current());
            double ratio = std::pow(2.0, semi / 12.0);
            mCachedAACoeff = (ratio > 1.05) ? std::min(0.9, 1.0 - 1.0 / ratio) : 0.0;
        }

        // Capture output peak after processing
        {
            double pkL = 0.0, pkR = 0.0;
            for (int32_t i = 0; i < frameCount; ++i) {
                double a = std::fabs(static_cast<double>(buffers[0][i]));
                if (a > pkL) pkL = a;
            }
            if (clampedChannels >= 2) {
                for (int32_t i = 0; i < frameCount; ++i) {
                    double a = std::fabs(static_cast<double>(buffers[1][i]));
                    if (a > pkR) pkR = a;
                }
            } else { pkR = pkL; }
            mOutputLevelL.store(pkL, std::memory_order_relaxed);
            mOutputLevelR.store(pkR, std::memory_order_relaxed);
        }
    }

    // ── Accessors for UI ─────────────────────────────────────────────────

    AutomationCurve* automationCurvePtr() { return &mAutomation; }

    uint64_t activeNoteBitmaskLo() const { return mNoteBitmaskLo.load(std::memory_order_relaxed); }
    uint64_t activeNoteBitmaskHi() const { return mNoteBitmaskHi.load(std::memory_order_relaxed); }
    double currentBeatPosition() const { return mBeatPosition.load(std::memory_order_relaxed); }
    double currentPitchSemitones() const { return mDisplayPitch.load(std::memory_order_relaxed); }

    double inputLevelL() const { return mInputLevelL.load(std::memory_order_relaxed); }
    double inputLevelR() const { return mInputLevelR.load(std::memory_order_relaxed); }
    double outputLevelL() const { return mOutputLevelL.load(std::memory_order_relaxed); }
    double outputLevelR() const { return mOutputLevelR.load(std::memory_order_relaxed); }

    /// Current auto-glide target in semitones (relative to C4). For UI display.
    double autoGlideTarget() const { return mAutoGlideTarget.load(std::memory_order_relaxed); }

    /// Latency in samples — depends on active shifter mode.
    int32_t latencySamples() const {
        if (mShifterMode == 1) return mFFTSize;
        return mGrainSamples;
    }

    /// Tail time in seconds — depends on active shifter mode.
    double tailTimeSeconds() const {
        int32_t lat = latencySamples();
        return (mSampleRate > 0.0) ? (static_cast<double>(lat) / mSampleRate) : 0.0;
    }

private:
    static constexpr int kMaxChannels = 2;

    int32_t mChannelCount = 2;
    double  mSampleRate   = 48000.0;
    int     mPitchRange   = 24;
    int     mShifterMode  = 0;  // 0=Granular, 1=Vocoder
    int     mAutoGlide    = 0;  // 0=Manual (curve), 1=Auto (MIDI-driven)

    // Auto-glide state
    static constexpr uint8_t kReferenceNote = 60;  // C4 = 0 semitones
    uint8_t mLastNoteOn = 0;
    std::atomic<double> mAutoGlideTarget{0.0};

    uint8_t findHighestActiveNote() const {
        for (int n = 127; n >= 0; --n) {
            if (mActiveNotes[n] > 0) return static_cast<uint8_t>(n);
        }
        return 0;
    }

    // Parameter smoothers
    ParameterSmoother mSmGlideTime;
    ParameterSmoother mSmMix;
    ParameterSmoother mSmPitchOffset;
    ParameterSmoother mSmPitch;
    double mLastGlideTimeMs = 50.0;

    // Anti-aliasing filter state
    double mAAFilterL = 0.0;
    double mAAFilterR = 0.0;
    double mCachedAACoeff = 0.0;  // computed once per block, not per sample

    // Granular pitch shifting
    GranularPitchShifter mGranularShifters[kMaxChannels];
    double* mGranularMemory = nullptr;
    int32_t mPitchBufSize = 0;
    int32_t mGrainSamples = 1440;

    // Phase vocoder pitch shifting
    PhaseVocoderPitchShifter mVocoderShifters[kMaxChannels];
    double* mVocoderMemory = nullptr;
    int32_t mFFTSize = PhaseVocoderPitchShifter::kFFTSize;

    // Automation curve (triple-buffered, lock-free)
    AutomationCurve mAutomation;

    // MIDI note state
    uint8_t mActiveNotes[128] = {};
    std::atomic<uint64_t> mNoteBitmaskLo{0};
    std::atomic<uint64_t> mNoteBitmaskHi{0};

    // Transport
    std::atomic<double> mBeatPosition{0.0};
    std::atomic<double> mTempo{120.0};

    // Display
    std::atomic<double> mDisplayPitch{0.0};
    std::atomic<double> mInputLevelL{0.0};
    std::atomic<double> mInputLevelR{0.0};
    std::atomic<double> mOutputLevelL{0.0};
    std::atomic<double> mOutputLevelR{0.0};
};
