#pragma once
//
// NoteTracker — tracks the set of currently-held MIDI notes.
//
// Used by MyRythem to know which pitches to re-gate on each rhythmic step.
// Fixed capacity (no allocations), RT-safe on the audio thread.
//
// Notes are stored in insertion order (so the chord reflects play order).
// Duplicate note-ons for the same pitch update the stored velocity but do
// not add a second entry. Note-off removes the entry. Count saturates at
// kMaxHeldNotes — extras are silently dropped (this will never matter for
// musical input, which is typically well under 20 simultaneous notes).
//

#include <cstdint>

namespace myrythem {

struct HeldNote {
    uint8_t pitch    = 0;   // MIDI note number 0..127
    uint8_t velocity = 0;   // MIDI velocity 1..127 (0 means empty slot)
    uint8_t channel  = 0;   // MIDI channel 0..15
};

class NoteTracker {
public:
    static constexpr int kMaxHeldNotes = 32;

    NoteTracker() { clear(); }

    /// Add a note (or update its velocity if already held).
    /// Returns true if the held-set changed (new pitch added or removed).
    bool noteOn(uint8_t pitch, uint8_t velocity, uint8_t channel = 0) {
        if (velocity == 0) return noteOff(pitch, channel);  // running status

        // Update existing
        for (int i = 0; i < mCount; ++i) {
            if (mNotes[i].pitch == pitch && mNotes[i].channel == channel) {
                mNotes[i].velocity = velocity;
                return false;  // already held, set unchanged
            }
        }
        // Insert new
        if (mCount >= kMaxHeldNotes) return false;
        mNotes[mCount] = { pitch, velocity, channel };
        ++mCount;
        return true;
    }

    /// Remove a note. Returns true if a note was actually removed.
    bool noteOff(uint8_t pitch, uint8_t channel = 0) {
        for (int i = 0; i < mCount; ++i) {
            if (mNotes[i].pitch == pitch && mNotes[i].channel == channel) {
                // Compact — shift remaining down by one
                for (int j = i; j < mCount - 1; ++j) {
                    mNotes[j] = mNotes[j + 1];
                }
                --mCount;
                mNotes[mCount] = {};
                return true;
            }
        }
        return false;
    }

    /// Drop every held note (e.g. on All Notes Off, or Latch clear).
    void clear() {
        for (int i = 0; i < kMaxHeldNotes; ++i) mNotes[i] = {};
        mCount = 0;
    }

    int count() const { return mCount; }
    bool empty() const { return mCount == 0; }

    const HeldNote& at(int i) const { return mNotes[i]; }

    /// Snapshot the current held set into an output buffer.
    /// Returns the number of notes copied (≤ maxOut).
    int snapshot(HeldNote* out, int maxOut) const {
        int n = (mCount < maxOut) ? mCount : maxOut;
        for (int i = 0; i < n; ++i) out[i] = mNotes[i];
        return n;
    }

    /// Average velocity across held notes (for accent scaling / display).
    /// Returns 0 when empty.
    float averageVelocity() const {
        if (mCount == 0) return 0.0f;
        int sum = 0;
        for (int i = 0; i < mCount; ++i) sum += mNotes[i].velocity;
        return static_cast<float>(sum) / static_cast<float>(mCount);
    }

private:
    HeldNote mNotes[kMaxHeldNotes];
    int      mCount = 0;
};

}  // namespace myrythem
