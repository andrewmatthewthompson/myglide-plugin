#include "GlideProcessor.hpp"
#include "AutomationCurve.hpp"
#include "GranularPitchShifter.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>

// ── Minimal test framework ──────────────────────────────────────────────

static int gTestsPassed = 0;
static int gTestsFailed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { \
            printf("  %-55s ", #name); \
            test_##name(); \
        } \
    } reg_##name; \
    static void test_##name()

#define EXPECT(cond) do { \
    if (cond) { ++gTestsPassed; printf("PASS\n"); } \
    else { ++gTestsFailed; printf("FAIL  (%s:%d)\n", __FILE__, __LINE__); } \
} while(0)

// ── Helpers ──────────────────────────────────────────────────────────────

static double rms(const float* buf, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += buf[i] * buf[i];
    return std::sqrt(sum / n);
}

static bool hasInvalidSamples(const float* buf, int n) {
    for (int i = 0; i < n; ++i)
        if (std::isnan(buf[i]) || std::isinf(buf[i])) return true;
    return false;
}

static double peakAbs(const float* buf, int n) {
    double peak = 0.0;
    for (int i = 0; i < n; ++i) {
        double a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

static double goertzel(const float* buf, int n, double targetFreq, double sampleRate) {
    double k = 0.5 + (double(n) * targetFreq / sampleRate);
    double w = 2.0 * M_PI * k / double(n);
    double coeff = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        s0 = buf[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / double(n);
}

struct StereoBuffer {
    static constexpr int kMaxFrames = 48000;
    float left[kMaxFrames]  = {};
    float right[kMaxFrames] = {};
    float* channels[2] = { left, right };
    void clear() {
        std::memset(left, 0, sizeof(left));
        std::memset(right, 0, sizeof(right));
    }
};

static void fillSine(float* buf, int n, double freq, double sampleRate, double amplitude = 0.5) {
    for (int i = 0; i < n; ++i)
        buf[i] = float(amplitude * std::sin(2.0 * M_PI * freq * i / sampleRate));
}

// ══════════════════════════════════════════════════════════════════════════
// AutomationCurve Tests
// ══════════════════════════════════════════════════════════════════════════

TEST(curve_empty_returns_zero) {
    AutomationCurve c;
    EXPECT(c.evaluate(5.0) == 0.0);
}

TEST(curve_single_breakpoint) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(4.0, 7.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 7.0 && c.evaluate(4.0) == 7.0 && c.evaluate(10.0) == 7.0);
}

TEST(curve_linear_interpolation) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(std::fabs(c.evaluate(2.0) - 6.0) < 0.01);
}

TEST(curve_smooth_interpolation) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Smooth);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Smooth);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(std::fabs(c.evaluate(2.0) - 6.0) < 0.01);
    double quarter = c.evaluate(1.0);
    EXPECT(std::fabs(quarter - 3.0) > 0.1);  // smooth differs from linear off-center
}

TEST(curve_step_interpolation) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Step);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Step);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(2.0) == 0.0 && c.evaluate(3.99) == 0.0);
}

TEST(curve_before_first_holds) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(4.0, 5.0);
    c.addBreakpoint(8.0, 10.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 5.0 && c.evaluate(3.0) == 5.0);
}

TEST(curve_after_last_holds) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 3.0);
    c.addBreakpoint(4.0, 7.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(10.0) == 7.0 && c.evaluate(100.0) == 7.0);
}

TEST(curve_multiple_segments) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Linear);
    c.addBreakpoint(8.0, -12.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(std::fabs(c.evaluate(2.0) - 6.0) < 0.01);
    EXPECT(std::fabs(c.evaluate(6.0) - 0.0) < 0.01);
}

TEST(curve_add_remove_breakpoint) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 5.0);
    c.addBreakpoint(4.0, 10.0);
    c.addBreakpoint(8.0, 15.0);
    c.removeBreakpoint(1);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(std::fabs(c.evaluate(4.0) - 10.0) < 0.01);
}

TEST(curve_move_breakpoint) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Linear);
    c.moveBreakpoint(1, 8.0, 24.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(std::fabs(c.evaluate(4.0) - 12.0) < 0.01);
}

TEST(curve_triple_buffer_swap) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 5.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 5.0);
    c.beginEdit();
    c.clearBreakpoints();
    c.addBreakpoint(0.0, 10.0);
    c.commitEdit();
    EXPECT(c.evaluate(0.0) == 5.0);  // before swap
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 10.0);  // after swap
}

TEST(curve_serialization_roundtrip) {
    AutomationCurve src;
    src.beginEdit();
    src.addBreakpoint(1.0, 3.0, InterpolationType::Linear);
    src.addBreakpoint(5.0, -7.0, InterpolationType::Smooth);
    src.addBreakpoint(9.0, 12.0, InterpolationType::Step);
    src.commitEdit();
    src.swapIfPending();

    uint8_t data[8192];
    int len = src.serialize(data, sizeof(data));
    EXPECT(len > 0);

    AutomationCurve dst;
    dst.beginEdit();
    dst.deserialize(data, len);
    dst.commitEdit();
    dst.swapIfPending();
    EXPECT(std::fabs(dst.evaluate(1.0) - 3.0) < 0.01);
    EXPECT(std::fabs(dst.evaluate(9.0) - 12.0) < 0.01);
}

// ── NEW: Boundary & edge case tests ─────────────────────────────────────

TEST(curve_max_breakpoints) {
    AutomationCurve c;
    c.beginEdit();
    for (int i = 0; i < AutomationCurve::kMaxBreakpoints; ++i) {
        EXPECT(c.addBreakpoint(double(i), double(i % 24 - 12)) >= 0);
    }
    // 257th should fail
    EXPECT(c.addBreakpoint(999.0, 0.0) == -1);
    c.commitEdit();
    c.swapIfPending();
    // Verify evaluate works with 256 breakpoints
    EXPECT(!std::isnan(c.evaluate(128.0)));
}

TEST(curve_coincident_beats) {
    // Two breakpoints at the same beat — should not crash or divide by zero
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(4.0, 0.0);
    c.addBreakpoint(4.0, 12.0);
    c.commitEdit();
    c.swapIfPending();
    double val = c.evaluate(4.0);
    EXPECT(!std::isnan(val) && !std::isinf(val));
}

TEST(curve_negative_beat_values) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(-4.0, -12.0);
    c.addBreakpoint(4.0, 12.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(std::fabs(c.evaluate(0.0) - 0.0) < 0.01);
    EXPECT(c.evaluate(-10.0) == -12.0);
}

TEST(curve_rapid_commit_swap_cycles) {
    AutomationCurve c;
    // Simulate rapid UI edits
    for (int round = 0; round < 50; ++round) {
        c.beginEdit();
        c.clearBreakpoints();
        c.addBreakpoint(0.0, double(round));
        c.commitEdit();
        c.swapIfPending();
    }
    EXPECT(std::fabs(c.evaluate(0.0) - 49.0) < 0.01);
}

TEST(curve_segment_cache_sequential) {
    // Verify that sequential evaluation benefits from segment cache
    AutomationCurve c;
    c.beginEdit();
    for (int i = 0; i < 100; ++i) {
        c.addBreakpoint(double(i), double(i % 12));
    }
    c.commitEdit();
    c.swapIfPending();
    // Sequential scan — should use cache after first lookup
    bool allValid = true;
    for (int i = 0; i < 9900; ++i) {
        double beat = double(i) * 0.01;
        double val = c.evaluate(beat);
        if (std::isnan(val) || std::isinf(val)) { allValid = false; break; }
    }
    EXPECT(allValid);
}

TEST(curve_remove_out_of_bounds) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 5.0);
    c.removeBreakpoint(-1);   // should not crash
    c.removeBreakpoint(100);  // should not crash
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 5.0);  // breakpoint still there
}

TEST(curve_breakpoint_struct_size) {
    EXPECT(sizeof(Breakpoint) == 24);
}

// ══════════════════════════════════════════════════════════════════════════
// GranularPitchShifter Tests
// ══════════════════════════════════════════════════════════════════════════

TEST(pitcher_passthrough_at_zero_semitones) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(0.0);

    float input[4800], output[4800];
    fillSine(input, 4800, 440.0, 48000.0);
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(double(input[i])));

    double inRms = rms(input, 4800);
    double outRms = rms(output + 1440, 3360);
    EXPECT(outRms > inRms * 0.3 && outRms < inRms * 2.0);
}

TEST(pitcher_zero_semitones_preserves_440hz) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(0.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));

    double mag440 = goertzel(output + 4800, 43200, 440.0, 48000.0);
    double mag880 = goertzel(output + 4800, 43200, 880.0, 48000.0);
    // 440Hz should dominate over 880Hz with no pitch shift
    EXPECT(mag440 > mag880 * 2.0);
}

TEST(pitcher_octave_up) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(12.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));

    double mag880 = goertzel(output + 4800, 43200, 880.0, 48000.0);
    double mag440 = goertzel(output + 4800, 43200, 440.0, 48000.0);
    EXPECT(mag880 > mag440 * 0.5);
}

TEST(pitcher_no_invalid_samples) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(7.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 48000));
}

TEST(pitcher_no_runaway) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(24.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(peakAbs(output, 48000) < 5.0);
}

TEST(pitcher_negative_semitones) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(-12.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 880.0 * i / 48000.0)));
    double mag440 = goertzel(output + 4800, 43200, 440.0, 48000.0);
    double mag880 = goertzel(output + 4800, 43200, 880.0, 48000.0);
    EXPECT(mag440 > mag880 * 0.3);
}

TEST(pitcher_rapid_pitch_changes) {
    // Sweep pitch from -24 to +24 semitones over 1 second — no crashes or NaN
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i) {
        double semi = -24.0 + 48.0 * (double(i) / 48000.0);
        ps.setPitchSemitones(semi);
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    }
    EXPECT(!hasInvalidSamples(output, 48000) && peakAbs(output, 48000) < 10.0);
}

TEST(pitcher_silence_stays_silent) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(12.0);

    float output[4800];
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(0.0));
    EXPECT(peakAbs(output, 4800) < 1e-10);
}

// ══════════════════════════════════════════════════════════════════════════
// GlideProcessor Tests
// ══════════════════════════════════════════════════════════════════════════

TEST(processor_silence_in_silence_out) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    StereoBuffer buf;
    p.process(buf.channels, 2, 4800);
    EXPECT(rms(buf.left, 4800) < 1e-4 && rms(buf.right, 4800) < 1e-4);
}

TEST(processor_passthrough_no_automation) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    p.setBeatPosition(0.0, 120.0);

    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);
    EXPECT(rms(buf.left, 4800) > 0.01);
}

TEST(processor_no_invalid_samples) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(0.0, 120.0);

    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 48000.0);
    fillSine(buf.right, 48000, 440.0, 48000.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000) && !hasInvalidSamples(buf.right, 48000));
}

TEST(processor_midi_note_tracking) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.handleMIDIEvent(0x90, 60, 100);
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) != 0);
    p.handleMIDIEvent(0x80, 60, 0);
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) == 0);
}

TEST(processor_midi_note_on_velocity_zero_is_off) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.handleMIDIEvent(0x90, 64, 100);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 0)) != 0);
    p.handleMIDIEvent(0x90, 64, 0);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 0)) == 0);
}

TEST(processor_parameter_set_and_get) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kGlideTime, 500.0f);
    p.setParameter(kMix, 75.0f);

    StereoBuffer buf;
    p.process(buf.channels, 2, 48000);
    EXPECT(std::fabs(p.getParameter(kGlideTime) - 500.0f) < 2.0f);
    EXPECT(std::fabs(p.getParameter(kMix) - 75.0f) < 2.0f);
}

TEST(processor_extreme_params_no_crash) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kGlideTime, 2000.0f);
    p.setParameter(kMix, 100.0f);
    p.setBeatPosition(0.0, 300.0);

    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 48000.0);
    fillSine(buf.right, 48000, 440.0, 48000.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000));
}

TEST(processor_beat_position_advances) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(0.0, 120.0);

    StereoBuffer buf;
    p.process(buf.channels, 2, 48000);
    double pos = p.currentBeatPosition();
    EXPECT(std::fabs(pos - 2.0) < 0.1);
}

// ── NEW: Integration & edge case tests ──────────────────────────────────

TEST(processor_dry_mix_preserves_input) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 0.0f);  // fully dry
    p.setBeatPosition(0.0, 120.0);

    // Let smoother converge to 0%
    StereoBuffer warmup;
    p.process(warmup.channels, 2, 48000);

    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    float originalPeak = float(peakAbs(buf.left, 4800));
    p.process(buf.channels, 2, 4800);

    // At 0% mix, output = dry input (unshifted)
    double mag440 = goertzel(buf.left, 4800, 440.0, 48000.0);
    EXPECT(mag440 > 0.05);
}

TEST(processor_setUp_tearDown_setUp) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.tearDown();
    p.setUp(2, 96000.0);  // re-setup at different rate

    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 96000.0);
    fillSine(buf.right, 48000, 440.0, 96000.0);
    p.setBeatPosition(0.0, 120.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000));
}

TEST(processor_process_before_setUp) {
    GlideProcessor p;
    // process without setUp — should not crash (null guard)
    StereoBuffer buf;
    fillSine(buf.left, 100, 440.0, 48000.0);
    p.process(buf.channels, 2, 100);
    EXPECT(true);  // didn't crash
}

TEST(processor_multiple_midi_notes_chord) {
    GlideProcessor p;
    p.setUp(2, 48000.0);

    // Play C major chord: C4(60), E4(64), G4(67)
    p.handleMIDIEvent(0x90, 60, 100);
    p.handleMIDIEvent(0x90, 64, 100);
    p.handleMIDIEvent(0x90, 67, 100);

    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) != 0);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 0)) != 0);   // 64
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 3)) != 0);   // 67

    // Release one note — others remain
    p.handleMIDIEvent(0x80, 64, 0);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 0)) == 0);
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) != 0);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 3)) != 0);
}

TEST(processor_midi_high_notes) {
    GlideProcessor p;
    p.setUp(2, 48000.0);

    // Note 127 (highest MIDI)
    p.handleMIDIEvent(0x90, 127, 100);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 63)) != 0);
    p.handleMIDIEvent(0x80, 127, 0);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 63)) == 0);
}

TEST(processor_automation_affects_pitch) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    p.setParameter(kGlideTime, 1.0f);  // very fast glide (nearly instant)
    p.setBeatPosition(0.0, 120.0);

    // Add +12 semitone automation at beat 0
    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 12.0, InterpolationType::Linear);
    curve->commitEdit();

    // Warm-up: let glideTime smoother converge from default 50ms to 1ms,
    // and let pitch smoother converge to +12 semitones
    StereoBuffer warmup;
    fillSine(warmup.left, 48000, 440.0, 48000.0);
    fillSine(warmup.right, 48000, 440.0, 48000.0);
    p.process(warmup.channels, 2, 48000);

    // Now process with smoothers fully converged
    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 48000.0);
    fillSine(buf.right, 48000, 440.0, 48000.0);
    p.process(buf.channels, 2, 48000);

    // Output should have 880Hz as the dominant frequency (octave up)
    double mag880 = goertzel(buf.left + 4800, 43200, 880.0, 48000.0);
    double mag440 = goertzel(buf.left + 4800, 43200, 440.0, 48000.0);
    EXPECT(mag880 > mag440);  // 880Hz should dominate over 440Hz
}

TEST(processor_96khz_no_crash) {
    GlideProcessor p;
    p.setUp(2, 96000.0);
    p.setBeatPosition(0.0, 120.0);

    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 96000.0);
    fillSine(buf.right, 48000, 440.0, 96000.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000));
}

TEST(processor_mono_channel) {
    GlideProcessor p;
    p.setUp(1, 48000.0);
    p.setBeatPosition(0.0, 120.0);

    float mono[48000];
    float* channels[1] = { mono };
    fillSine(mono, 48000, 440.0, 48000.0);
    p.process(channels, 1, 48000);
    EXPECT(!hasInvalidSamples(mono, 48000));
}

// ══════════════════════════════════════════════════════════════════════════
// Performance Benchmark
// ══════════════════════════════════════════════════════════════════════════

static void runBenchmark() {
    printf("\n  Performance Benchmark\n");
    printf("  ─────────────────────────────────────────────────────────\n");

    const double sampleRate = 48000.0;
    const int seconds = 10;
    const int totalFrames = int(sampleRate) * seconds;
    const int blockSize = 512;

    auto benchConfig = [&](const char* name, double mix, double glideMs,
                           bool withAutomation, double pitchSemi) {
        GlideProcessor p;
        p.setUp(2, sampleRate);
        p.setParameter(kMix, float(mix));
        p.setParameter(kGlideTime, float(glideMs));
        p.setBeatPosition(0.0, 120.0);

        if (withAutomation) {
            auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
            curve->beginEdit();
            curve->addBreakpoint(0.0, 0.0, InterpolationType::Smooth);
            curve->addBreakpoint(4.0, pitchSemi, InterpolationType::Smooth);
            curve->addBreakpoint(8.0, 0.0, InterpolationType::Smooth);
            curve->addBreakpoint(12.0, -pitchSemi, InterpolationType::Smooth);
            curve->addBreakpoint(16.0, 0.0, InterpolationType::Smooth);
            curve->commitEdit();
        }

        // Prepare input
        float left[blockSize], right[blockSize];
        float* channels[2] = { left, right };
        fillSine(left, blockSize, 440.0, sampleRate);
        fillSine(right, blockSize, 440.0, sampleRate);

        auto start = std::chrono::high_resolution_clock::now();

        int framesProcessed = 0;
        while (framesProcessed < totalFrames) {
            // Reset input each block (simulate real audio)
            fillSine(left, blockSize, 440.0, sampleRate);
            fillSine(right, blockSize, 440.0, sampleRate);
            p.process(channels, 2, blockSize);
            framesProcessed += blockSize;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        double samplesPerSec = double(totalFrames) / (ms * 0.001);
        double realtimeX = samplesPerSec / sampleRate;
        double cpuPercent = 100.0 / realtimeX;

        printf("  %-35s  %6.1fx realtime   %5.2f%% CPU\n", name, realtimeX, cpuPercent);
    };

    benchConfig("Bypass (0% mix)",          0.0,   50.0, false, 0.0);
    benchConfig("Default (100% mix, flat)", 100.0,  50.0, false, 0.0);
    benchConfig("Automation (+12 glide)",   100.0,  50.0, true,  12.0);
    benchConfig("Automation (+24 glide)",   100.0,  50.0, true,  24.0);
    benchConfig("Fast glide (1ms)",         100.0,   1.0, true,  12.0);
    benchConfig("Slow glide (2000ms)",      100.0, 2000.0, true, 12.0);
}

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
    printf("\nMyGlide DSP Tests\n");
    printf("=================\n\n");

    // Tests run via static initialization before main()

    printf("\nResults: %d passed, %d failed\n", gTestsPassed, gTestsFailed);

    runBenchmark();

    printf("\n");
    return gTestsFailed > 0 ? 1 : 0;
}
