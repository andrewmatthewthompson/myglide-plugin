#pragma once
//
// RythemProcessor — MyRythem's MIDI processing core.
//
// Intakes MIDI (note-on / note-off), tracks the held chord, and emits a
// re-gated version of that chord at each active step of the selected
// pattern. All host/DSP coupling happens through a tiny API called from
// the Obj-C++ bridge.
//
// Not an arpeggiator: on each step, ALL held notes are re-triggered
// simultaneously. Rhythm is created by the step pattern, not by note
// ordering.
//
// This header is free of Apple/C++17-only dependencies (no AudioToolbox
// types) so it can be unit-tested standalone with `c++ -std=c++17`.
//

#include "NoteTracker.hpp"
#include "PatternBank.hpp"
#include "StepClock.hpp"

#include <cstdint>
#include <cmath>

namespace myrythem {

/// Parameter addresses — keep in sync with RythemParameters.swift Address enum.
enum ParamAddress : uint64_t {
    kParamPattern       = 0,   // PatternId (0..9)
    kParamRate          = 1,   // RateId (0..5)
    kParamGate          = 2,   // % of step (5..100)
    kParamSwing         = 3,   // swing % (50..75)
    kParamVelocityMode  = 4,   // 0=Fixed, 1=FromInput, 2=Accented
    kParamFixedVelocity = 5,   // 1..127
    kParamAccent        = 6,   // %
    kParamOctave        = 7,   // -2..+2
    kParamLatch         = 8,   // 0/1
    kParamSyncMode      = 9,   // 0=Host, 1=Free
    kParamFreeBPM       = 10,  // 30..300
    kParamReleaseMode   = 11,  // 0=Immediate, 1=FinishBar
    kParamPatternLength = 12,  // 1..16 (used for Custom)
    kParamCount
};

/// One scheduled MIDI event to emit within the current render block.
struct ScheduledMIDI {
    uint8_t  status;       // 0x90 = NoteOn, 0x80 = NoteOff (channel in low nibble)
    uint8_t  data1;        // note number
    uint8_t  data2;        // velocity
    uint32_t frameOffset;  // sample offset within the current block
};

class RythemProcessor {
public:
    static constexpr int kMaxPendingEvents = 128;   // per-block output capacity

    // ─── Lifecycle ──────────────────────────────────────────────────────────

    void setUp(int32_t /*channelCount*/, double sampleRate) {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        mClock.reset();
        mPendingCount = 0;
        mActiveGatedCount = 0;
        for (int i = 0; i < kMaxHeldGated; ++i) mActiveGated[i] = {};
    }

    void tearDown() {
        killAllSounding(0);
        mHeld.clear();
        mLatched.clear();
        mPendingCount = 0;
    }

    // ─── Parameters ─────────────────────────────────────────────────────────

    void setParameter(uint64_t address, float value) {
        switch (address) {
            case kParamPattern:       mPattern       = clampi(value, 0, (int)PatternId::Count - 1); break;
            case kParamRate:          mRate          = clampi(value, 0, (int)RateId::Count - 1); break;
            case kParamGate:          mGatePct       = clampf(value, 5.0f, 100.0f); break;
            case kParamSwing:         mSwingPct      = clampf(value, 50.0f, 75.0f); break;
            case kParamVelocityMode:  mVelocityMode  = clampi(value, 0, 2); break;
            case kParamFixedVelocity: mFixedVelocity = clampi(value, 1, 127); break;
            case kParamAccent:        mAccentPct     = clampf(value, 0.0f, 100.0f); break;
            case kParamOctave:        mOctave        = clampi(value, -2, 2); break;
            case kParamLatch: {
                int newLatch = value > 0.5f ? 1 : 0;
                if (newLatch != mLatchOn) {
                    mLatchOn = newLatch;
                    if (!mLatchOn) mLatched.clear();
                }
                break;
            }
            case kParamSyncMode:      mSyncMode      = clampi(value, 0, 1); break;
            case kParamFreeBPM:       mFreeBPM       = clampf(value, 30.0f, 300.0f); break;
            case kParamReleaseMode:   mReleaseMode   = clampi(value, 0, 1); break;
            case kParamPatternLength: mPatternLen    = clampi(value, 1, 16); break;
            default: break;
        }
    }

    float getParameter(uint64_t address) const {
        switch (address) {
            case kParamPattern:       return (float)mPattern;
            case kParamRate:          return (float)mRate;
            case kParamGate:          return mGatePct;
            case kParamSwing:         return mSwingPct;
            case kParamVelocityMode:  return (float)mVelocityMode;
            case kParamFixedVelocity: return (float)mFixedVelocity;
            case kParamAccent:        return mAccentPct;
            case kParamOctave:        return (float)mOctave;
            case kParamLatch:         return (float)mLatchOn;
            case kParamSyncMode:      return (float)mSyncMode;
            case kParamFreeBPM:       return mFreeBPM;
            case kParamReleaseMode:   return (float)mReleaseMode;
            case kParamPatternLength: return (float)mPatternLen;
            default: return 0.0f;
        }
    }

    /// Custom-pattern bitmask (bit i = step i active). Used when Pattern=Custom.
    void setCustomPatternMask(uint32_t mask) { mCustomMask = mask; }
    uint32_t customPatternMask() const       { return mCustomMask; }

    // ─── Host transport ─────────────────────────────────────────────────────

    /// Called once per render block with the current transport state.
    void setBeatPosition(double beatPos, double tempo) {
        mHostBeatPos = beatPos;
        if (tempo > 0.0) mHostTempo = tempo;
    }

    /// Called when transport jumps (loop, locate). Resyncs the step clock
    /// so we don't double-fire the same step.
    void onTransportJump() {
        mClock.reset();
        killAllSounding(0);
    }

    // ─── MIDI input ─────────────────────────────────────────────────────────

    /// Feed an input MIDI event. Does not emit output — the render block
    /// is responsible for pulling scheduled events out via pendingEvents().
    void handleMIDIEvent(uint8_t status, uint8_t data1, uint8_t data2,
                         uint32_t /*frameOffset*/ = 0) {
        uint8_t type    = status & 0xF0;
        uint8_t channel = status & 0x0F;

        if (type == 0x90 && data2 > 0) {
            // Note on
            if (mLatchOn) {
                // In latch mode: first press after full release clears,
                // subsequent presses while still held add to the set.
                if (mHeld.empty() && !mLatched.empty() && !mLatchCarryOver) {
                    mLatched.clear();
                }
                mLatched.noteOn(data1, data2, channel);
                mLatchCarryOver = true;
            }
            mHeld.noteOn(data1, data2, channel);
        } else if (type == 0x80 || (type == 0x90 && data2 == 0)) {
            // Note off
            mHeld.noteOff(data1, channel);
            if (mHeld.empty()) mLatchCarryOver = false;
        } else if (type == 0xB0 && data1 == 123) {
            // All Notes Off (CC#123)
            mHeld.clear();
            mLatched.clear();
            mLatchCarryOver = false;
            killAllSounding(0);
        }
    }

    // ─── Per-block step / MIDI scheduling ───────────────────────────────────

    /// Advance the step clock by `frameCount` samples and enqueue any
    /// output MIDI events that fall within this block.
    ///
    /// Must be called once per render block, AFTER setBeatPosition() and
    /// AFTER all handleMIDIEvent() calls for this block.
    void process(int32_t frameCount) {
        mPendingCount = 0;

        if (frameCount <= 0) return;

        // Effective tempo for "Free" sync (when host transport is stopped).
        double tempo = (mSyncMode == 0) ? mHostTempo : (double)mFreeBPM;
        if (tempo <= 0.0) tempo = 120.0;

        // In Free mode, advance our own internal beat position.
        double blockBeats = (tempo / 60.0) * ((double)frameCount / mSampleRate);
        double startBeat, endBeat;
        if (mSyncMode == 1) {
            startBeat = mFreeBeatPos;
            endBeat   = mFreeBeatPos + blockBeats;
            mFreeBeatPos = endBeat;
        } else {
            startBeat = mHostBeatPos;
            endBeat   = mHostBeatPos + blockBeats;
        }

        // Determine the step subdivision in beats.
        double stepBeats = 1.0 / subdivisionsPerBeat((RateId)mRate);
        if (stepBeats <= 0.0) stepBeats = 0.25;

        // Find every step-boundary that falls inside [startBeat, endBeat).
        // For each, enqueue note-off for currently-sounding gated notes
        // (if any) and note-on for the held set (if the step is active).
        Pattern pat = currentPattern();
        if (pat.length == 0) pat.length = 1;

        // Ceil-division: first step index >= startBeat.
        int64_t firstStep = (int64_t)std::ceil(startBeat / stepBeats - 1e-9);
        // Last step index < endBeat.
        int64_t lastStep  = (int64_t)std::floor(endBeat   / stepBeats - 1e-9);

        // Also handle the gate-off events for steps we are currently
        // "inside" — i.e. the last triggered step's gate length has
        // elapsed. We model each currently-sounding gated note with an
        // absolute gate-off beat.
        emitPendingGateOffs(startBeat, endBeat, frameCount);

        for (int64_t step = firstStep; step <= lastStep; ++step) {
            double stepBeat = (double)step * stepBeats;
            // Swing offset on odd steps
            if ((step & 1) == 1) {
                double sFrac = ((double)mSwingPct - 50.0) / 50.0;
                if (sFrac < 0.0) sFrac = 0.0;
                if (sFrac > 0.5) sFrac = 0.5;
                stepBeat += sFrac * 0.5 * stepBeats;
                if (stepBeat >= endBeat) continue;
                if (stepBeat <  startBeat) continue;
            }

            uint32_t frameOffset = beatToFrameOffset(stepBeat, startBeat, frameCount);

            // Kill any still-sounding gated notes from the previous step
            // (unless gate is 100% AND this step is also active — legato).
            bool stepFires = stepActiveForIndex(pat, step);
            if (stepFires && mGatePct >= 99.99f) {
                // legato — don't insert note-offs; but still need to avoid
                // stacking duplicate note-ons on the same pitch. Emit offs
                // for any sounding note that ISN'T in the current held set.
                emitGateOffsForStaleNotes(frameOffset);
            } else {
                emitAllGateOffs(frameOffset);
            }

            if (!stepFires) continue;

            // Choose source note set: held (live) or latched.
            const NoteTracker& src = (mLatchOn && !mLatched.empty()) ? mLatched : mHeld;
            if (src.empty()) continue;

            // Accent: beat-1-of-pattern gets boosted velocity.
            bool isAccent = ((step % pat.length) == 0);

            for (int i = 0; i < src.count(); ++i) {
                const HeldNote& n = src.at(i);
                int pitch = (int)n.pitch + 12 * mOctave;
                if (pitch < 0 || pitch > 127) continue;
                uint8_t vel = computeVelocity(n.velocity, isAccent);
                pushEvent(0x90, (uint8_t)pitch, vel, frameOffset);

                // Schedule gate-off at stepBeat + gateFrac * stepBeats.
                double gateOffBeat = stepBeat + ((double)mGatePct / 100.0) * stepBeats;
                registerSoundingNote((uint8_t)pitch, gateOffBeat);
            }
        }

        // Update the externally-visible clock state (for UI).
        mClock.update(endBeat, (RateId)mRate, mSwingPct);
    }

    // ─── Output queue accessors ─────────────────────────────────────────────

    int              pendingCount() const          { return mPendingCount; }
    const ScheduledMIDI& pendingAt(int i) const    { return mPending[i]; }

    // ─── UI / display state ─────────────────────────────────────────────────

    int  heldNoteCount() const     { return mHeld.count(); }
    int  latchedNoteCount() const  { return mLatched.count(); }
    int  currentStepIndex() const  { return (int)(mClock.stepIndex() % 16); }
    bool stepIsActiveNow() const   { return stepActiveForIndex(currentPattern(), mClock.stepIndex()); }

    // ─── Kill-all (bypass / reset) ──────────────────────────────────────────

    /// Emit note-offs for every currently-sounding gated note.
    /// `frameOffset` is the sample offset in the *next* block where these
    /// kill events will be output.
    void killAllSounding(uint32_t frameOffset) {
        for (int i = 0; i < mActiveGatedCount; ++i) {
            pushEvent(0x80, mActiveGated[i].pitch, 0, frameOffset);
        }
        mActiveGatedCount = 0;
    }

private:
    // ─── Internal state ─────────────────────────────────────────────────────

    static constexpr int kMaxHeldGated = NoteTracker::kMaxHeldNotes;

    struct SoundingNote {
        uint8_t pitch       = 0;
        double  gateOffBeat = 0.0;   // absolute host beat for note-off
    };

    double mSampleRate = 48000.0;
    double mHostBeatPos = 0.0;
    double mHostTempo   = 120.0;
    double mFreeBeatPos = 0.0;

    NoteTracker mHeld;
    NoteTracker mLatched;
    bool        mLatchCarryOver = false;

    // Parameters (see ParamAddress)
    int   mPattern       = (int)PatternId::EighthSixteenth;
    int   mRate          = (int)RateId::Sixteenth;
    float mGatePct       = 60.0f;
    float mSwingPct      = 50.0f;
    int   mVelocityMode  = 1;       // FromInput default
    int   mFixedVelocity = 100;
    float mAccentPct     = 30.0f;
    int   mOctave        = 0;
    int   mLatchOn       = 0;
    int   mSyncMode      = 0;
    float mFreeBPM       = 120.0f;
    int   mReleaseMode   = 0;
    int   mPatternLen    = 16;
    uint32_t mCustomMask = 0;

    StepClock mClock;

    // Output queue
    ScheduledMIDI mPending[kMaxPendingEvents];
    int           mPendingCount = 0;

    // Currently-sounding gated notes (so we can emit note-offs)
    SoundingNote mActiveGated[kMaxHeldGated];
    int          mActiveGatedCount = 0;

    // ─── Helpers ────────────────────────────────────────────────────────────

    static int clampi(float v, int lo, int hi) {
        int i = (int)std::lround(v);
        if (i < lo) i = lo;
        if (i > hi) i = hi;
        return i;
    }
    static float clampf(float v, float lo, float hi) {
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        return v;
    }

    Pattern currentPattern() const {
        if ((PatternId)mPattern == PatternId::Custom) {
            uint8_t len = (uint8_t)((mPatternLen < 1) ? 1 : (mPatternLen > 32 ? 32 : mPatternLen));
            return { mCustomMask, len, 4 };
        }
        return builtinPattern((PatternId)mPattern);
    }

    static bool stepActiveForIndex(const Pattern& p, int64_t stepIndex) {
        if (p.length == 0) return false;
        int64_t idx = stepIndex % p.length;
        if (idx < 0) idx += p.length;
        return (p.mask >> idx) & 1u;
    }

    uint32_t beatToFrameOffset(double targetBeat, double startBeat, int32_t frameCount) const {
        double tempo = (mSyncMode == 0) ? mHostTempo : (double)mFreeBPM;
        if (tempo <= 0.0) tempo = 120.0;
        double beatsPerSample = (tempo / 60.0) / mSampleRate;
        if (beatsPerSample <= 0.0) return 0;
        double offsetBeats = targetBeat - startBeat;
        double offsetSamples = offsetBeats / beatsPerSample;
        if (offsetSamples < 0.0) offsetSamples = 0.0;
        if (offsetSamples > (double)(frameCount - 1)) offsetSamples = (double)(frameCount - 1);
        return (uint32_t)offsetSamples;
    }

    uint8_t computeVelocity(uint8_t inputVel, bool isAccent) const {
        float v = 100.0f;
        switch (mVelocityMode) {
            case 0: v = (float)mFixedVelocity; break;                      // Fixed
            case 1: v = (float)inputVel; break;                            // FromInput
            case 2: {                                                       // Accented
                v = (float)inputVel;
                if (isAccent) {
                    // Push toward 127 by accent strength.
                    float headroom = 127.0f - v;
                    v += headroom * (mAccentPct / 100.0f);
                } else {
                    // Slightly reduce non-accent hits.
                    v *= 1.0f - 0.25f * (mAccentPct / 100.0f);
                }
                break;
            }
        }
        if (v < 1.0f) v = 1.0f;
        if (v > 127.0f) v = 127.0f;
        return (uint8_t)std::lround(v);
    }

    void pushEvent(uint8_t status, uint8_t data1, uint8_t data2, uint32_t frameOffset) {
        if (mPendingCount >= kMaxPendingEvents) return;
        mPending[mPendingCount++] = { status, data1, data2, frameOffset };
    }

    void registerSoundingNote(uint8_t pitch, double gateOffBeat) {
        // Dedupe: if already sounding, just update the gate-off time.
        for (int i = 0; i < mActiveGatedCount; ++i) {
            if (mActiveGated[i].pitch == pitch) {
                mActiveGated[i].gateOffBeat = gateOffBeat;
                return;
            }
        }
        if (mActiveGatedCount >= kMaxHeldGated) return;
        mActiveGated[mActiveGatedCount++] = { pitch, gateOffBeat };
    }

    void emitPendingGateOffs(double startBeat, double endBeat, int32_t frameCount) {
        int w = 0;
        for (int r = 0; r < mActiveGatedCount; ++r) {
            if (mActiveGated[r].gateOffBeat >= startBeat && mActiveGated[r].gateOffBeat < endBeat) {
                uint32_t off = beatToFrameOffset(mActiveGated[r].gateOffBeat, startBeat, frameCount);
                pushEvent(0x80, mActiveGated[r].pitch, 0, off);
                // drop
            } else if (mActiveGated[r].gateOffBeat >= endBeat) {
                mActiveGated[w++] = mActiveGated[r];
            } else {
                // gate-off was in the past (shouldn't happen if we processed
                // contiguous blocks, but be defensive): emit at block start.
                pushEvent(0x80, mActiveGated[r].pitch, 0, 0);
            }
        }
        mActiveGatedCount = w;
    }

    void emitAllGateOffs(uint32_t frameOffset) {
        for (int i = 0; i < mActiveGatedCount; ++i) {
            pushEvent(0x80, mActiveGated[i].pitch, 0, frameOffset);
        }
        mActiveGatedCount = 0;
    }

    void emitGateOffsForStaleNotes(uint32_t frameOffset) {
        const NoteTracker& src = (mLatchOn && !mLatched.empty()) ? mLatched : mHeld;
        int w = 0;
        for (int r = 0; r < mActiveGatedCount; ++r) {
            uint8_t pitch = mActiveGated[r].pitch;
            bool stillHeld = false;
            for (int i = 0; i < src.count(); ++i) {
                int shifted = (int)src.at(i).pitch + 12 * mOctave;
                if (shifted == pitch) { stillHeld = true; break; }
            }
            if (stillHeld) {
                mActiveGated[w++] = mActiveGated[r];
            } else {
                pushEvent(0x80, pitch, 0, frameOffset);
            }
        }
        mActiveGatedCount = w;
    }
};

}  // namespace myrythem
