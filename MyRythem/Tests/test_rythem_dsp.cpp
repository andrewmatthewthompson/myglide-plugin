// MyRythem DSP tests — standalone, no Xcode / no AudioToolbox required.
//
//   c++ -std=c++17 -O2 -I ../MyRythemAU/DSP test_rythem_dsp.cpp -o test_rythem
//   ./test_rythem
//
// Or from the repo root:
//   c++ -std=c++17 -O2 -I MyRythem/MyRythemAU/DSP MyRythem/Tests/test_rythem_dsp.cpp -o MyRythem/Tests/test_rythem
//   ./MyRythem/Tests/test_rythem

#include "NoteTracker.hpp"
#include "PatternBank.hpp"
#include "StepClock.hpp"
#include "RythemProcessor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace myrythem;

static int gChecks = 0;
static int gFails  = 0;

#define CHECK(expr) do { \
    ++gChecks; \
    if (!(expr)) { \
        ++gFails; \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    ++gChecks; \
    auto _a = (a); auto _b = (b); \
    if (!(_a == _b)) { \
        ++gFails; \
        std::fprintf(stderr, "FAIL [%s:%d]: %s (%lld) == %s (%lld)\n", \
                     __FILE__, __LINE__, #a, (long long)_a, #b, (long long)_b); \
    } \
} while (0)

// ── NoteTracker ─────────────────────────────────────────────────────────────

static void test_note_tracker_basic() {
    NoteTracker t;
    CHECK(t.empty());
    CHECK_EQ(t.count(), 0);

    CHECK(t.noteOn(60, 100));
    CHECK(t.noteOn(64, 90));
    CHECK(t.noteOn(67, 80));
    CHECK_EQ(t.count(), 3);

    // Duplicate note-on updates velocity, doesn't add entry
    CHECK(!t.noteOn(64, 110));
    CHECK_EQ(t.count(), 3);
    CHECK_EQ(t.at(1).velocity, 110);

    // Note-off with velocity 0 (running status)
    CHECK(t.noteOn(60, 0));   // == noteOff(60)
    CHECK_EQ(t.count(), 2);
    CHECK_EQ(t.at(0).pitch, 64);

    // Note-off for non-held pitch returns false
    CHECK(!t.noteOff(99));
    CHECK_EQ(t.count(), 2);

    t.clear();
    CHECK(t.empty());
}

static void test_note_tracker_capacity() {
    NoteTracker t;
    for (int i = 0; i < NoteTracker::kMaxHeldNotes + 5; ++i) {
        t.noteOn((uint8_t)(50 + i), 100);
    }
    CHECK_EQ(t.count(), NoteTracker::kMaxHeldNotes);
}

static void test_note_tracker_channels() {
    NoteTracker t;
    t.noteOn(60, 100, 0);
    t.noteOn(60, 100, 1);
    CHECK_EQ(t.count(), 2);  // different channels = different entries
    t.noteOff(60, 0);
    CHECK_EQ(t.count(), 1);
    CHECK_EQ(t.at(0).channel, 1);
}

// ── PatternBank ─────────────────────────────────────────────────────────────

static void test_pattern_bank_straight() {
    Pattern p = builtinPattern(PatternId::Straight8ths);
    CHECK_EQ(p.length, 16);
    // Every even step is active, odd silent.
    for (int i = 0; i < 16; ++i) {
        CHECK_EQ(stepActive(p, i), (i & 1) == 0);
    }
}

static void test_pattern_bank_sixteenths() {
    Pattern p = builtinPattern(PatternId::Straight16ths);
    for (int i = 0; i < 16; ++i) CHECK(stepActive(p, i));
}

static void test_pattern_bank_dotted8ths() {
    Pattern p = builtinPattern(PatternId::Dotted8ths);
    CHECK_EQ(p.length, 3);
    // Only step 0 of the 3-step window fires; others silent.
    CHECK(stepActive(p, 0));
    CHECK(!stepActive(p, 1));
    CHECK(!stepActive(p, 2));
    CHECK(stepActive(p, 3));   // wraps
    CHECK(stepActive(p, 6));
}

static void test_pattern_bank_dotted8plus16() {
    Pattern p = builtinPattern(PatternId::Dotted8thPlus16th);
    CHECK_EQ(p.length, 4);
    // Mask 0b1001 → steps 0 and 3 fire
    CHECK(stepActive(p, 0));
    CHECK(!stepActive(p, 1));
    CHECK(!stepActive(p, 2));
    CHECK(stepActive(p, 3));
    CHECK(stepActive(p, 4));   // wraps → step 0
}

static void test_pattern_bank_eighth_sixteenth() {
    Pattern p = builtinPattern(PatternId::EighthSixteenth);
    // Per-beat (4 sixteenths): 1 0 1 1 repeating.
    for (int beat = 0; beat < 4; ++beat) {
        CHECK(stepActive(p, beat * 4 + 0));   // ♪
        CHECK(!stepActive(p, beat * 4 + 1));
        CHECK(stepActive(p, beat * 4 + 2));   // ♬ first
        CHECK(stepActive(p, beat * 4 + 3));   // ♬ second
    }
}

// ── StepClock ───────────────────────────────────────────────────────────────

static void test_step_clock_rates() {
    CHECK_EQ(subdivisionsPerBeat(RateId::Quarter),    1.0);
    CHECK_EQ(subdivisionsPerBeat(RateId::Eighth),     2.0);
    CHECK_EQ(subdivisionsPerBeat(RateId::Sixteenth),  4.0);
    CHECK_EQ(subdivisionsPerBeat(RateId::ThirtySecond), 8.0);
}

static void test_step_clock_advance() {
    StepClock c;
    c.reset();
    // At 1/16 rate, stepBeats=0.25. beatPos=0 → step 0.
    bool crossed = c.update(0.0, RateId::Sixteenth, 50.0f);
    CHECK(crossed);
    CHECK_EQ(c.stepIndex(), 0);

    // Still step 0.
    CHECK(!c.update(0.24, RateId::Sixteenth, 50.0f));
    CHECK_EQ(c.stepIndex(), 0);

    // Cross into step 1.
    CHECK(c.update(0.25, RateId::Sixteenth, 50.0f));
    CHECK_EQ(c.stepIndex(), 1);

    // Cross into step 4 (next beat).
    CHECK(c.update(1.0, RateId::Sixteenth, 50.0f));
    CHECK_EQ(c.stepIndex(), 4);
}

static void test_step_clock_swing() {
    StepClock c; c.reset();
    // 66% swing pushes odd steps by (66-50)/50 * 0.5 * 0.25 = 0.04 beats.
    // At beat 0.25 exactly (no swing would be step 1), we should STILL be step 0.
    c.update(0.0, RateId::Sixteenth, 66.0f);
    CHECK_EQ(c.stepIndex(), 0);
    c.update(0.25, RateId::Sixteenth, 66.0f);
    CHECK_EQ(c.stepIndex(), 0);   // hasn't crossed odd step 1 yet
    c.update(0.30, RateId::Sixteenth, 66.0f);
    CHECK_EQ(c.stepIndex(), 1);
}

// ── RythemProcessor: end-to-end MIDI scheduling ─────────────────────────────

static void test_processor_emits_chord_on_step() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::Straight16ths);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);
    p.setParameter(kParamGate,    50.0f);

    // Hold a C-major triad
    p.handleMIDIEvent(0x90, 60, 100);
    p.handleMIDIEvent(0x90, 64, 100);
    p.handleMIDIEvent(0x90, 67, 100);
    CHECK_EQ(p.heldNoteCount(), 3);

    // Process one 512-frame block at 120bpm, starting beat 0.
    p.setBeatPosition(0.0, 120.0);
    p.process(512);

    // Block length at 120bpm / 48kHz / 512 frames = 0.0213 beats.
    // At 1/16 rate (stepBeats=0.25) we should hit exactly one step boundary (step 0).
    // Expect 3 note-ons (no offs yet since gate hasn't elapsed).
    int noteOns = 0, noteOffs = 0;
    for (int i = 0; i < p.pendingCount(); ++i) {
        const auto& e = p.pendingAt(i);
        if ((e.status & 0xF0) == 0x90) ++noteOns;
        if ((e.status & 0xF0) == 0x80) ++noteOffs;
    }
    CHECK_EQ(noteOns, 3);
    CHECK_EQ(noteOffs, 0);
}

static void test_processor_gate_off_fires() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::QuarterPulse);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);
    p.setParameter(kParamGate,    50.0f);   // 50% gate → off at mid-step

    p.handleMIDIEvent(0x90, 60, 100);

    // Process a big block that covers step 0 AND the gate-off for it.
    // Block is 0.3 beats long → exceeds 0.25 + 0.125 (gate-off).
    // 0.3 beats at 120bpm = 0.15 sec = 7200 samples at 48k.
    p.setBeatPosition(0.0, 120.0);
    p.process(7200);

    int noteOns = 0, noteOffs = 0;
    for (int i = 0; i < p.pendingCount(); ++i) {
        const auto& e = p.pendingAt(i);
        if ((e.status & 0xF0) == 0x90) ++noteOns;
        if ((e.status & 0xF0) == 0x80) ++noteOffs;
    }
    CHECK_EQ(noteOns, 1);     // only step 0 is active in QuarterPulse
    CHECK_EQ(noteOffs, 1);    // and its gate-off fired within this block
}

static void test_processor_no_notes_no_output() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::Straight16ths);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);

    p.setBeatPosition(0.0, 120.0);
    p.process(512);
    CHECK_EQ(p.pendingCount(), 0);
}

static void test_processor_octave_transpose() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::Straight16ths);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);
    p.setParameter(kParamOctave,  1.0f);  // +1 oct

    p.handleMIDIEvent(0x90, 60, 100);  // C4
    p.setBeatPosition(0.0, 120.0);
    p.process(512);

    bool foundC5 = false;
    for (int i = 0; i < p.pendingCount(); ++i) {
        const auto& e = p.pendingAt(i);
        if ((e.status & 0xF0) == 0x90 && e.data1 == 72) foundC5 = true;
    }
    CHECK(foundC5);
}

static void test_processor_chord_change_mid_pattern() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::Straight16ths);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);
    p.setParameter(kParamGate,    99.0f);   // near-legato

    // Chord 1: C major
    p.handleMIDIEvent(0x90, 60, 100);
    p.handleMIDIEvent(0x90, 64, 100);
    p.handleMIDIEvent(0x90, 67, 100);

    p.setBeatPosition(0.0, 120.0);
    p.process(512);       // fires step 0 with C major

    // Release the C and add Db → chord becomes E, G, Db (voicing change)
    p.handleMIDIEvent(0x80, 60, 0);
    p.handleMIDIEvent(0x90, 61, 100);

    // Advance to step 1 (beat 0.25 onward).
    p.setBeatPosition(0.25, 120.0);
    p.process(512);

    // Expect a note-off for pitch 60 (stale) and a note-on for pitch 61 (new).
    bool sawOffC = false, sawOnDb = false;
    for (int i = 0; i < p.pendingCount(); ++i) {
        const auto& e = p.pendingAt(i);
        if ((e.status & 0xF0) == 0x80 && e.data1 == 60) sawOffC = true;
        if ((e.status & 0xF0) == 0x90 && e.data1 == 61) sawOnDb = true;
    }
    CHECK(sawOffC);
    CHECK(sawOnDb);
}

static void test_processor_all_notes_off() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::Straight16ths);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);

    p.handleMIDIEvent(0x90, 60, 100);
    p.handleMIDIEvent(0x90, 64, 100);
    CHECK_EQ(p.heldNoteCount(), 2);

    // CC#123 = All Notes Off
    p.handleMIDIEvent(0xB0, 123, 0);
    CHECK_EQ(p.heldNoteCount(), 0);
}

static void test_processor_latch_mode() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamLatch, 1.0f);

    p.handleMIDIEvent(0x90, 60, 100);
    p.handleMIDIEvent(0x80, 60, 0);    // physical release
    // Note should stay latched.
    CHECK_EQ(p.latchedNoteCount(), 1);

    // Now play a new note — the latched set should be replaced.
    p.handleMIDIEvent(0x90, 64, 100);
    CHECK_EQ(p.latchedNoteCount(), 1);
    p.handleMIDIEvent(0x80, 64, 0);
    CHECK_EQ(p.latchedNoteCount(), 1);

    // Turning latch OFF clears the latched set.
    p.setParameter(kParamLatch, 0.0f);
    CHECK_EQ(p.latchedNoteCount(), 0);
}

static void test_processor_custom_pattern() {
    RythemProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kParamPattern, (float)PatternId::Custom);
    p.setParameter(kParamPatternLength, 4.0f);
    p.setParameter(kParamRate,    (float)RateId::Sixteenth);
    p.setCustomPatternMask(0b1010);   // steps 1 and 3 active
    p.handleMIDIEvent(0x90, 60, 100);

    // Run a full "bar" of 4 sixteenths (1 beat total = 0.5 sec at 120bpm).
    // 24000 samples at 48k = 0.5 sec = 1.0 beats.
    p.setBeatPosition(0.0, 120.0);
    p.process(24000);
    // Expect exactly 2 note-ons for pitch 60 in this block (steps 1 and 3).
    int ons = 0;
    for (int i = 0; i < p.pendingCount(); ++i) {
        const auto& e = p.pendingAt(i);
        if ((e.status & 0xF0) == 0x90 && e.data1 == 60) ++ons;
    }
    CHECK_EQ(ons, 2);
}

// ────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("MyRythem DSP tests\n");
    std::printf("==================\n");

    test_note_tracker_basic();
    test_note_tracker_capacity();
    test_note_tracker_channels();

    test_pattern_bank_straight();
    test_pattern_bank_sixteenths();
    test_pattern_bank_dotted8ths();
    test_pattern_bank_dotted8plus16();
    test_pattern_bank_eighth_sixteenth();

    test_step_clock_rates();
    test_step_clock_advance();
    test_step_clock_swing();

    test_processor_emits_chord_on_step();
    test_processor_gate_off_fires();
    test_processor_no_notes_no_output();
    test_processor_octave_transpose();
    test_processor_chord_change_mid_pattern();
    test_processor_all_notes_off();
    test_processor_latch_mode();
    test_processor_custom_pattern();

    std::printf("\n%d checks run, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
