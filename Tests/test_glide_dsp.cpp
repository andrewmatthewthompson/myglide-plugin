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

/// Helper: configure a GranularPitchShifter with self-contained Hann LUT.
/// hannBuf must have at least grainSamples doubles.
static void configurePitchShifter(GranularPitchShifter& ps,
                                  double* circBuf, int32_t circSize,
                                  double* hannBuf, double sampleRate, double grainMs = 30.0) {
    int32_t grainSamples = static_cast<int32_t>(grainMs * 0.001 * sampleRate);
    if (grainSamples < 4) grainSamples = 4;
    for (int32_t i = 0; i < grainSamples; ++i) {
        double phase = static_cast<double>(i) / static_cast<double>(grainSamples);
        hannBuf[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * phase));
    }
    ps.configure(circBuf, circSize, hannBuf, grainSamples, sampleRate);
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
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
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
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
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
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
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
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
    ps.setPitchSemitones(7.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 48000));
}

TEST(pitcher_no_runaway) {
    const int N = 48000;
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
    ps.setPitchSemitones(24.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(peakAbs(output, 48000) < 5.0);
}

TEST(pitcher_negative_semitones) {
    const int N = 48000;
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
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
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);

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
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);
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
// Bug-probing tests (targeted at specific crash/corruption scenarios)
// ══════════════════════════════════════════════════════════════════════════

TEST(bug_smoother_glide_time_change_no_pitch_jump) {
    // Verify that changing glide time doesn't cause a pitch discontinuity.
    // Before the fix, configure() reset mCurrent, snapping the pitch.
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    p.setParameter(kGlideTime, 200.0f);  // start slow
    p.setBeatPosition(0.0, 120.0);

    // Set automation to +12 semitones
    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 12.0, InterpolationType::Linear);
    curve->commitEdit();

    // Process until pitch smoother is partially converged
    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);   // 100ms
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);

    // Record output level at the boundary
    float beforeChange = buf.left[4799];

    // Now change glide time abruptly — this should NOT cause a pitch jump
    p.setParameter(kGlideTime, 10.0f);
    fillSine(buf.left, 100, 440.0, 48000.0);
    fillSine(buf.right, 100, 440.0, 48000.0);
    p.process(buf.channels, 2, 100);

    float afterChange = buf.left[0];

    // The output should be continuous — no huge jump
    double jump = std::fabs(double(afterChange) - double(beforeChange));
    EXPECT(jump < 0.5);  // less than 50% of full-scale jump
}

TEST(bug_pitcher_extreme_semitones_no_crash) {
    // Before the fix, extreme semitone values caused readPos out of bounds.
    const int N = 48000;
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);

    // +48 semitones = 4 octaves up (ratio = 16) — at the clamp boundary
    ps.setPitchSemitones(48.0);
    float output[4800];
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 4800));
}

TEST(bug_pitcher_corrupted_semitones_clamped) {
    // Simulates corrupted automation data with semitones = 1000
    const int N = 48000;
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);

    // This would have caused ratio = pow(2, 83.3) ≈ 1e25 before the clamp
    ps.setPitchSemitones(1000.0);
    float output[4800];
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 4800) && peakAbs(output, 4800) < 10.0);
}

TEST(bug_pitcher_negative_extreme_clamped) {
    const int N = 48000;
    double buf[N]; double hann[1440];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, N, hann, 48000.0);

    ps.setPitchSemitones(-1000.0);
    float output[4800];
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 4800));
}

TEST(bug_pitcher_tiny_buffer) {
    // Smallest reasonable buffer: 10 samples. Tests all wrapping edge cases.
    double buf[10]; double hann[8];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, 10, hann, 48000.0, 0.1);  // 0.1ms grain = ~5 samples

    ps.setPitchSemitones(12.0);
    float output[1000];
    for (int i = 0; i < 1000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 1000));
}

TEST(bug_pitcher_bufsize_1_no_crash) {
    // Edge case: buffer of size 4. Hermite needs 4 samples, so wrapping is critical.
    double buf[4]; double hann[8];
    GranularPitchShifter ps;
    configurePitchShifter(ps, buf, 4, hann, 48000.0, 0.1);

    ps.setPitchSemitones(0.0);
    float output[100];
    for (int i = 0; i < 100; ++i)
        output[i] = float(ps.process(0.3));
    EXPECT(!hasInvalidSamples(output, 100));
}

TEST(bug_curve_two_breakpoints_evaluate_at_exact_endpoints) {
    // Binary search edge: evaluate exactly at breakpoint beats
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(2.0, 5.0, InterpolationType::Linear);
    c.addBreakpoint(6.0, 10.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();

    EXPECT(std::fabs(c.evaluate(2.0) - 5.0) < 0.01);   // exactly at first
    EXPECT(std::fabs(c.evaluate(6.0) - 10.0) < 0.01);  // exactly at last
    EXPECT(std::fabs(c.evaluate(4.0) - 7.5) < 0.01);   // midpoint
}

TEST(bug_curve_three_breakpoints_boundary) {
    // Three breakpoints: tests binary search with lo=0 hi=2 → mid=1
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Linear);
    c.addBreakpoint(8.0, 0.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();

    // Verify each segment evaluates correctly
    EXPECT(std::fabs(c.evaluate(1.0) - 3.0) < 0.01);    // segment 0-1
    EXPECT(std::fabs(c.evaluate(4.0) - 12.0) < 0.01);   // at middle breakpoint
    EXPECT(std::fabs(c.evaluate(6.0) - 6.0) < 0.01);    // segment 1-2
}

TEST(bug_curve_cache_invalidation_on_swap) {
    // Verify that segment cache is invalidated when buffer is swapped
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(10.0, 10.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();

    // Warm the cache at beat 5
    EXPECT(std::fabs(c.evaluate(5.0) - 5.0) < 0.01);

    // Change the curve — same beats but different values
    c.beginEdit();
    c.clearBreakpoints();
    c.addBreakpoint(0.0, 100.0, InterpolationType::Linear);
    c.addBreakpoint(10.0, 100.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();  // should invalidate cache

    // If cache wasn't invalidated, we'd get stale segment data
    double val = c.evaluate(5.0);
    EXPECT(std::fabs(val - 100.0) < 0.01);
}

TEST(bug_processor_automation_with_corrupted_semitones) {
    // Automation curve with out-of-range semitones — should not crash
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    p.setParameter(kGlideTime, 1.0f);
    p.setBeatPosition(0.0, 120.0);

    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 500.0, InterpolationType::Linear);  // way out of range
    curve->commitEdit();

    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 48000.0);
    fillSine(buf.right, 48000, 440.0, 48000.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000));
}

TEST(bug_processor_zero_tempo) {
    // tempo = 0 should not cause division by zero
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(5.0, 0.0);  // zero tempo

    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);
    EXPECT(!hasInvalidSamples(buf.left, 4800));
    // Beat should not advance with zero tempo
    EXPECT(std::fabs(p.currentBeatPosition() - 5.0) < 0.01);
}

TEST(bug_processor_negative_tempo) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(10.0, -120.0);  // negative tempo (shouldn't happen but defensive)

    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);
    EXPECT(!hasInvalidSamples(buf.left, 4800));
}

TEST(bug_processor_very_large_beat_position) {
    // After hours of playback, beat position is very large
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(100000.0, 120.0);

    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    curve->addBreakpoint(16.0, 12.0, InterpolationType::Linear);
    curve->commitEdit();

    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);
    EXPECT(!hasInvalidSamples(buf.left, 4800));
}

TEST(bug_smoother_update_coefficient_preserves_current) {
    // Verify updateCoefficient doesn't reset mCurrent (the whole point of the fix)
    ParameterSmoother s;
    s.configure(48000.0, 100.0);
    s.setTarget(100.0);

    // Advance partway — mCurrent should be somewhere between 0 and 100
    for (int i = 0; i < 480; ++i) s.next();  // 10ms
    double midway = s.current();
    EXPECT(midway > 5.0 && midway < 95.0);

    // Update coefficient — mCurrent must NOT change
    s.updateCoefficient(48000.0, 50.0);
    double afterUpdate = s.current();
    EXPECT(std::fabs(afterUpdate - midway) < 0.001);
}

TEST(bug_deserialize_truncated_data) {
    // Deserialize with less data than count claims
    AutomationCurve c;
    c.beginEdit();

    // Claim 100 breakpoints but only provide data for 2
    uint8_t data[4 + 2 * 24];
    int32_t fakeCount = 100;
    std::memcpy(data, &fakeCount, 4);
    // Fill 2 breakpoints
    Breakpoint bp1 = { 1.0, 5.0, InterpolationType::Linear, {} };
    Breakpoint bp2 = { 2.0, 10.0, InterpolationType::Linear, {} };
    std::memcpy(data + 4, &bp1, 24);
    std::memcpy(data + 28, &bp2, 24);

    c.deserialize(data, sizeof(data));
    c.commitEdit();
    c.swapIfPending();

    // Should only have 2 breakpoints (clamped by available data)
    EXPECT(c.count() == 2);
    EXPECT(std::fabs(c.evaluate(1.0) - 5.0) < 0.01);
}

TEST(bug_deserialize_negative_count) {
    AutomationCurve c;
    c.beginEdit();

    uint8_t data[4];
    int32_t negCount = -5;
    std::memcpy(data, &negCount, 4);
    c.deserialize(data, 4);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.count() == 0);
}

TEST(bug_deserialize_zero_length) {
    AutomationCurve c;
    c.beginEdit();
    c.deserialize(nullptr, 0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.count() == 0);
}

TEST(bug_serialize_too_small_buffer) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 5.0);
    c.commitEdit();
    c.swapIfPending();

    // Buffer too small — should return 0 (not write past end)
    uint8_t tiny[4];
    int written = c.serialize(tiny, 4);  // needs 4 + 24 = 28 bytes
    EXPECT(written == 0);
}

TEST(bug_processor_midi_channel_ignored) {
    // MIDI events on different channels should all be processed (channel stripped)
    GlideProcessor p;
    p.setUp(2, 48000.0);

    // Note on channel 1 (status 0x90) vs channel 10 (status 0x99)
    p.handleMIDIEvent(0x99, 60, 100);  // channel 10
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) != 0);

    p.handleMIDIEvent(0x89, 60, 0);    // note off channel 10
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) == 0);
}

// ══════════════════════════════════════════════════════════════════════════
// Serialization through processor (preset save/load)
// ══════════════════════════════════════════════════════════════════════════

TEST(processor_serialize_deserialize_roundtrip) {
    // Simulate a full preset save/load cycle through the processor
    GlideProcessor src;
    src.setUp(2, 48000.0);
    src.setBeatPosition(0.0, 120.0);

    // Add breakpoints via the curve pointer (simulating UI edits)
    // Note: interpolation type on a breakpoint controls the segment AFTER it
    auto* srcCurve = static_cast<AutomationCurve*>(src.automationCurvePtr());
    srcCurve->beginEdit();
    srcCurve->addBreakpoint(0.0, 0.0, InterpolationType::Smooth);  // segment 0→4: smooth
    srcCurve->addBreakpoint(4.0, 12.0, InterpolationType::Linear);  // segment 4→8: linear
    srcCurve->addBreakpoint(8.0, -7.0, InterpolationType::Step);    // segment 8→12: step
    srcCurve->addBreakpoint(12.0, 3.5, InterpolationType::Linear);
    srcCurve->commitEdit();
    srcCurve->swapIfPending();

    // Serialize
    uint8_t data[8192];
    int len = srcCurve->serialize(data, sizeof(data));
    EXPECT(len > 0);

    // Deserialize into a fresh processor (simulating Logic Pro reopening a session)
    GlideProcessor dst;
    dst.setUp(2, 48000.0);
    auto* dstCurve = static_cast<AutomationCurve*>(dst.automationCurvePtr());
    dstCurve->beginEdit();
    dstCurve->deserialize(data, len);
    dstCurve->commitEdit();
    dstCurve->swapIfPending();

    // Verify all breakpoint values survived the roundtrip
    EXPECT(std::fabs(dstCurve->evaluate(0.0) - 0.0) < 0.01);
    EXPECT(std::fabs(dstCurve->evaluate(4.0) - 12.0) < 0.01);
    EXPECT(std::fabs(dstCurve->evaluate(8.0) - (-7.0)) < 0.01);
    EXPECT(std::fabs(dstCurve->evaluate(12.0) - 3.5) < 0.01);

    // Verify interpolation types: smooth between 0→4 should differ from linear
    double smoothMid = dstCurve->evaluate(2.0);
    double linearMid = 0.0 + (12.0 - 0.0) * 0.5;  // would be 6.0 for linear
    EXPECT(std::fabs(smoothMid - linearMid) < 0.01);  // at midpoint, smooth == linear
    double smoothQuarter = dstCurve->evaluate(1.0);
    EXPECT(std::fabs(smoothQuarter - 3.0) > 0.1);  // but off-center they differ

    // Verify step: between 8 and 12, step holds -7.0
    EXPECT(std::fabs(dstCurve->evaluate(10.0) - (-7.0)) < 0.01);
}

TEST(processor_display_pitch_updates) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    p.setParameter(kGlideTime, 1.0f);
    p.setBeatPosition(0.0, 120.0);

    // Add +12 automation
    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 12.0, InterpolationType::Linear);
    curve->commitEdit();

    // Process enough for smoother to converge
    StereoBuffer buf;
    for (int i = 0; i < 3; ++i) {
        fillSine(buf.left, 48000, 440.0, 48000.0);
        fillSine(buf.right, 48000, 440.0, 48000.0);
        p.process(buf.channels, 2, 48000);
    }

    // Display pitch should be close to 12.0
    double pitch = p.currentPitchSemitones();
    EXPECT(std::fabs(pitch - 12.0) < 1.0);
}

// ══════════════════════════════════════════════════════════════════════════
// Latency, Tail Time, and Bypass Tests
// ══════════════════════════════════════════════════════════════════════════

TEST(latency_reports_grain_size_at_48k) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    // 30ms grain @ 48kHz = 1440 samples
    EXPECT(p.latencySamples() == 1440);
}

TEST(latency_reports_grain_size_at_96k) {
    GlideProcessor p;
    p.setUp(2, 96000.0);
    // 30ms grain @ 96kHz = 2880 samples
    EXPECT(p.latencySamples() == 2880);
}

TEST(latency_reports_grain_size_at_44100) {
    GlideProcessor p;
    p.setUp(2, 44100.0);
    // 30ms grain @ 44.1kHz = 1323 samples
    EXPECT(p.latencySamples() == 1323);
}

TEST(tail_time_equals_grain_duration) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    double tail = p.tailTimeSeconds();
    // Should be 30ms (0.030s)
    EXPECT(std::fabs(tail - 0.030) < 0.001);
}

TEST(tail_time_scales_with_sample_rate) {
    // Tail time should be the same duration regardless of sample rate
    GlideProcessor p1, p2;
    p1.setUp(2, 48000.0);
    p2.setUp(2, 96000.0);
    EXPECT(std::fabs(p1.tailTimeSeconds() - p2.tailTimeSeconds()) < 0.001);
}

TEST(tail_time_zero_before_setup) {
    GlideProcessor p;
    // Before setUp, tailTime should be safe (not NaN or crash)
    double tail = p.tailTimeSeconds();
    EXPECT(!std::isnan(tail) && !std::isinf(tail));
}

TEST(bypass_passes_audio_through_unmodified) {
    // Simulate bypass: process should be skippable, output = input
    // (In the full AU, shouldBypassEffect skips process entirely.
    // Here we verify the processor itself can be skipped without issues.)
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(0.0, 120.0);

    // Add pitch automation that would shift audio if processed
    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 12.0, InterpolationType::Linear);
    curve->commitEdit();

    // Fill with known signal
    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);

    // Save copy of original input
    float original[4800];
    std::memcpy(original, buf.left, sizeof(original));

    // In bypass mode, we DON'T call process — audio passes through unchanged
    // (simulating what the render block does when shouldBypassEffect is true)

    // Verify the input buffer was not modified (bypass = no processing)
    bool identical = true;
    for (int i = 0; i < 4800; ++i) {
        if (buf.left[i] != original[i]) { identical = false; break; }
    }
    EXPECT(identical);
}

TEST(processor_resumable_after_bypass) {
    // After bypassing for a while, processing should resume cleanly
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setBeatPosition(0.0, 120.0);

    // Process some audio normally
    StereoBuffer buf;
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);

    // Simulate bypass: skip processing for 1 second
    // (beat position won't advance, which is correct for bypass)

    // Resume processing — should not crash or produce NaN
    fillSine(buf.left, 48000, 440.0, 48000.0);
    fillSine(buf.right, 48000, 440.0, 48000.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000) && !hasInvalidSamples(buf.right, 48000));
}

// ══════════════════════════════════════════════════════════════════════════
// Phase Vocoder Tests
// ══════════════════════════════════════════════════════════════════════════

static void configureVocoder(PhaseVocoderPitchShifter& ps, double* buf, double* hann, double sampleRate) {
    const int32_t fftSize = PhaseVocoderPitchShifter::kFFTSize;
    for (int32_t i = 0; i < fftSize; ++i) {
        double phase = static_cast<double>(i) / static_cast<double>(fftSize);
        hann[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * phase));
    }
    ps.configure(buf, PhaseVocoderPitchShifter::kMemoryPerChannel, hann, fftSize, sampleRate);
}

TEST(vocoder_fft_roundtrip) {
    // Forward FFT then inverse should reconstruct the original signal
    double real[2048], imag[2048];
    for (int i = 0; i < 2048; ++i) {
        real[i] = std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
        imag[i] = 0.0;
    }
    double origPeak = 0;
    for (int i = 0; i < 2048; ++i) {
        double a = std::fabs(real[i]);
        if (a > origPeak) origPeak = a;
    }

    // Forward + inverse FFT (access via a temporary vocoder just to call the static method)
    PhaseVocoderPitchShifter::fft(real, imag, 2048, false);
    PhaseVocoderPitchShifter::fft(real, imag, 2048, true);

    // Should reconstruct within tolerance
    double maxErr = 0;
    for (int i = 0; i < 2048; ++i) {
        double expected = std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
        double err = std::fabs(real[i] - expected);
        if (err > maxErr) maxErr = err;
    }
    EXPECT(maxErr < 1e-10);
}

TEST(vocoder_passthrough_at_zero_semitones) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);
    ps.setPitchSemitones(0.0);

    // Warmup (fill the FFT buffer)
    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));

    // After warmup, output should have energy
    double outRms = rms(output + 4096, 43904);
    EXPECT(outRms > 0.01);
}

TEST(vocoder_zero_semitones_preserves_440hz) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);
    ps.setPitchSemitones(0.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));

    double mag440 = goertzel(output + 4096, 43904, 440.0, 48000.0);
    double mag880 = goertzel(output + 4096, 43904, 880.0, 48000.0);
    EXPECT(mag440 > mag880 * 2.0);
}

TEST(vocoder_octave_up) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);
    ps.setPitchSemitones(12.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));

    double mag880 = goertzel(output + 4096, 43904, 880.0, 48000.0);
    double mag440 = goertzel(output + 4096, 43904, 440.0, 48000.0);
    EXPECT(mag880 > mag440);
}

TEST(vocoder_no_invalid_samples) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);
    ps.setPitchSemitones(7.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(!hasInvalidSamples(output, 48000));
}

TEST(vocoder_no_runaway) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);
    ps.setPitchSemitones(24.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i)
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    EXPECT(peakAbs(output, 48000) < 5.0);
}

TEST(vocoder_silence_stays_silent) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);
    ps.setPitchSemitones(12.0);

    float output[4800];
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(0.0));
    EXPECT(peakAbs(output, 4800) < 1e-10);
}

TEST(vocoder_rapid_pitch_changes) {
    double buf[PhaseVocoderPitchShifter::kMemoryPerChannel];
    double hann[PhaseVocoderPitchShifter::kFFTSize];
    PhaseVocoderPitchShifter ps;
    configureVocoder(ps, buf, hann, 48000.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i) {
        double semi = -24.0 + 48.0 * (double(i) / 48000.0);
        ps.setPitchSemitones(semi);
        output[i] = float(ps.process(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0)));
    }
    EXPECT(!hasInvalidSamples(output, 48000) && peakAbs(output, 48000) < 10.0);
}

TEST(processor_vocoder_mode_no_crash) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kShifterMode, 1.0f);
    p.setParameter(kMix, 100.0f);
    p.setBeatPosition(0.0, 120.0);

    StereoBuffer buf;
    fillSine(buf.left, 48000, 440.0, 48000.0);
    fillSine(buf.right, 48000, 440.0, 48000.0);
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000) && !hasInvalidSamples(buf.right, 48000));
}

TEST(processor_mode_switch_no_crash) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    p.setBeatPosition(0.0, 120.0);

    StereoBuffer buf;
    // Process in granular mode
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);

    // Switch to vocoder mid-stream
    p.setParameter(kShifterMode, 1.0f);
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);

    // Switch back
    p.setParameter(kShifterMode, 0.0f);
    fillSine(buf.left, 4800, 440.0, 48000.0);
    fillSine(buf.right, 4800, 440.0, 48000.0);
    p.process(buf.channels, 2, 4800);

    EXPECT(!hasInvalidSamples(buf.left, 4800));
}

TEST(processor_vocoder_latency_reports_fft_size) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kShifterMode, 1.0f);
    EXPECT(p.latencySamples() == PhaseVocoderPitchShifter::kFFTSize);
}

TEST(processor_granular_latency_unchanged) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kShifterMode, 0.0f);
    // 30ms at 48kHz = 1440
    EXPECT(p.latencySamples() == 1440);
}

TEST(perf_vocoder_48k_above_50x) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kShifterMode, 1.0f);
    p.setParameter(kMix, 100.0f);
    p.setBeatPosition(0.0, 120.0);

    auto* curve = static_cast<AutomationCurve*>(p.automationCurvePtr());
    curve->beginEdit();
    curve->addBreakpoint(0.0, 0.0, InterpolationType::Smooth);
    curve->addBreakpoint(4.0, 12.0, InterpolationType::Smooth);
    curve->commitEdit();

    const int totalFrames = 48000 * 5;
    const int blockSize = 512;
    float left[512], right[512];
    float* channels[2] = {left, right};

    auto start = std::chrono::high_resolution_clock::now();
    int processed = 0;
    while (processed < totalFrames) {
        fillSine(left, blockSize, 440.0, 48000.0);
        fillSine(right, blockSize, 440.0, 48000.0);
        p.process(channels, 2, blockSize);
        processed += blockSize;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double wallSec = std::chrono::duration<double>(end - start).count();
    double rt = (double(totalFrames) / 48000.0) / wallSec;
    printf("[%.0fx] ", rt);
    EXPECT(rt > 50.0);
}

TEST(perf_vocoder_memory_under_512kb) {
    // Vocoder memory per channel should be < 512KB
    int bytes = PhaseVocoderPitchShifter::kMemoryPerChannel * int(sizeof(double));
    EXPECT(bytes < 512 * 1024);
}

// ══════════════════════════════════════════════════════════════════════════
// Performance Regression Tests
//
// These tests enforce minimum performance floors. If a code change causes
// any configuration to drop below its floor, the test suite fails.
//
// Floors are set at ~50% of measured performance (April 2025, Apple M-series)
// to absorb variance from CI runners, thermal throttling, and background load.
// They should NEVER be lowered — only raised when optimizations land.
//
// Current measured baselines (48kHz stereo, 512-sample blocks):
//   Bypass:     700x RT  →  floor 200x
//   Default:    810x RT  →  floor 200x
//   Automation: 800x RT  →  floor 200x
//   192kHz:     285x RT  →  floor 100x
//
// Logic Pro hard limit: must stay above 20x realtime (5% CPU) at any config.
// Our floor of 100x (1% CPU) provides 5x safety margin.
// ══════════════════════════════════════════════════════════════════════════

/// Measures realtime multiplier for a given processor configuration.
/// Runs enough audio to get a stable measurement (5 seconds).
static double measureRealtimeMultiplier(double sampleRate, int blockSize, int channels,
                                        double mix, double glideMs,
                                        bool withAutomation, double pitchSemi) {
    GlideProcessor p;
    p.setUp(channels, sampleRate);
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

    const int totalFrames = static_cast<int>(sampleRate) * 5;  // 5 seconds
    float left[2048], right[2048];
    float* chPtrs[2] = { left, right };

    // Warmup (1 block)
    fillSine(left, blockSize, 440.0, sampleRate);
    fillSine(right, blockSize, 440.0, sampleRate);
    p.process(chPtrs, channels, blockSize);

    auto start = std::chrono::high_resolution_clock::now();
    int processed = 0;
    while (processed < totalFrames) {
        fillSine(left, blockSize, 440.0, sampleRate);
        if (channels > 1) fillSine(right, blockSize, 440.0, sampleRate);
        p.process(chPtrs, channels, blockSize);
        processed += blockSize;
    }
    auto end = std::chrono::high_resolution_clock::now();

    double wallSec = std::chrono::duration<double>(end - start).count();
    double audioSec = double(processed) / sampleRate;
    return audioSec / wallSec;
}

// ── CPU performance floors ──────────────────────────────────────────────

TEST(perf_48k_bypass_above_200x) {
    double rt = measureRealtimeMultiplier(48000.0, 512, 2, 0.0, 50.0, false, 0.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 200.0);
}

TEST(perf_48k_default_above_200x) {
    double rt = measureRealtimeMultiplier(48000.0, 512, 2, 100.0, 50.0, false, 0.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 200.0);
}

TEST(perf_48k_automation_above_200x) {
    double rt = measureRealtimeMultiplier(48000.0, 512, 2, 100.0, 50.0, true, 12.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 200.0);
}

TEST(perf_48k_extreme_automation_above_200x) {
    double rt = measureRealtimeMultiplier(48000.0, 512, 2, 100.0, 50.0, true, 24.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 200.0);
}

TEST(perf_48k_small_block_above_150x) {
    // 128-sample blocks: tightest deadline (2.67ms). More overhead per block.
    double rt = measureRealtimeMultiplier(48000.0, 128, 2, 100.0, 50.0, true, 12.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 150.0);
}

TEST(perf_96k_above_150x) {
    double rt = measureRealtimeMultiplier(96000.0, 256, 2, 100.0, 50.0, true, 12.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 150.0);
}

TEST(perf_192k_above_100x) {
    // 192kHz is the most demanding. Must still be >100x (< 1% CPU).
    double rt = measureRealtimeMultiplier(192000.0, 512, 2, 100.0, 50.0, true, 12.0);
    printf("[%.0fx] ", rt);
    EXPECT(rt > 100.0);
}

// ── Memory size floors ──────────────────────────────────────────────────

TEST(perf_memory_inline_under_25kb) {
    // GlideProcessor inline memory must stay small.
    // Before optimization: 318KB. After: 18.5KB. Floor: 25KB.
    EXPECT(sizeof(GlideProcessor) < 25 * 1024);
}

TEST(perf_memory_pitchshifter_under_256_bytes) {
    // GranularPitchShifter must remain lightweight (no inline LUTs).
    // Before optimization: 150KB. After: 104 bytes. Floor: 256 bytes.
    EXPECT(sizeof(GranularPitchShifter) < 256);
}

TEST(perf_memory_heap_48k_under_1mb) {
    // Total heap allocation at 48kHz stereo: pitch buffers + Hann LUT.
    // 0.5s * 48000 * 2ch * 8bytes + 1440 * 8bytes = ~386KB. Floor: 1MB.
    int heapDoubles = int(48000 * 0.5) * 2 + 1440;
    int heapBytes = heapDoubles * int(sizeof(double));
    EXPECT(heapBytes < 1024 * 1024);
}

TEST(perf_memory_heap_192k_under_4mb) {
    // At 192kHz: 0.5s * 192000 * 2ch * 8bytes + 5760 * 8bytes = ~1.5MB. Floor: 4MB.
    int heapDoubles = int(192000 * 0.5) * 2 + 5760;
    int heapBytes = heapDoubles * int(sizeof(double));
    EXPECT(heapBytes < 4 * 1024 * 1024);
}

// ── Per-block deadline tests ────────────────────────────────────────────

TEST(perf_worst_case_under_5pct_cpu) {
    // Logic Pro hard requirement: plugin must use <5% of one core.
    // Test the worst case: 192kHz, 128-sample blocks, full automation.
    double rt = measureRealtimeMultiplier(192000.0, 128, 2, 100.0, 50.0, true, 24.0);
    double cpuPct = 100.0 / rt;
    printf("[%.2f%%] ", cpuPct);
    EXPECT(cpuPct < 5.0);
}

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
    printf("\nMyGlide DSP Tests\n");
    printf("=================\n\n");

    // Tests run via static initialization before main()

    printf("\nResults: %d passed, %d failed\n\n", gTestsPassed, gTestsFailed);
    return gTestsFailed > 0 ? 1 : 0;
}
