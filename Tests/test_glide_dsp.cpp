#include "GlideProcessor.hpp"
#include "AutomationCurve.hpp"
#include "GranularPitchShifter.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>

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

/// Goertzel algorithm: returns magnitude at target frequency.
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

// Fill buffer with a sine wave
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
    // Before, at, and after the breakpoint should return its value
    EXPECT(c.evaluate(0.0) == 7.0 && c.evaluate(4.0) == 7.0 && c.evaluate(10.0) == 7.0);
}

TEST(curve_linear_interpolation) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Linear);
    c.commitEdit();
    c.swapIfPending();
    double mid = c.evaluate(2.0);
    EXPECT(std::fabs(mid - 6.0) < 0.01);
}

TEST(curve_smooth_interpolation) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Smooth);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Smooth);
    c.commitEdit();
    c.swapIfPending();
    double mid = c.evaluate(2.0);
    // Smooth (smoothstep): at t=0.5, should be 6.0 (same as linear at midpoint)
    EXPECT(std::fabs(mid - 6.0) < 0.01);
    // But at t=0.25, smooth should differ from linear
    double quarter = c.evaluate(1.0);  // t = 0.25
    double linearQuarter = 3.0;
    EXPECT(std::fabs(quarter - linearQuarter) > 0.1);  // smooth differs from linear off-center
}

TEST(curve_step_interpolation) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Step);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Step);
    c.commitEdit();
    c.swapIfPending();
    // Step holds the first value until the next breakpoint
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
    c.removeBreakpoint(1);  // remove middle
    c.commitEdit();
    c.swapIfPending();
    // Now just 2 breakpoints: (0,5) and (8,15)
    double mid = c.evaluate(4.0);  // halfway
    EXPECT(std::fabs(mid - 10.0) < 0.01);
}

TEST(curve_move_breakpoint) {
    AutomationCurve c;
    c.beginEdit();
    c.addBreakpoint(0.0, 0.0, InterpolationType::Linear);
    c.addBreakpoint(4.0, 12.0, InterpolationType::Linear);
    c.moveBreakpoint(1, 8.0, 24.0);
    c.commitEdit();
    c.swapIfPending();
    // Now (0,0) → (8,24)
    EXPECT(std::fabs(c.evaluate(4.0) - 12.0) < 0.01);
}

TEST(curve_triple_buffer_swap) {
    AutomationCurve c;
    // First commit
    c.beginEdit();
    c.addBreakpoint(0.0, 5.0);
    c.commitEdit();
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 5.0);

    // Second commit (new value)
    c.beginEdit();
    c.clearBreakpoints();
    c.addBreakpoint(0.0, 10.0);
    c.commitEdit();
    // Before swap, old value
    EXPECT(c.evaluate(0.0) == 5.0);
    // After swap, new value
    c.swapIfPending();
    EXPECT(c.evaluate(0.0) == 10.0);
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

// ══════════════════════════════════════════════════════════════════════════
// GranularPitchShifter Tests
// ══════════════════════════════════════════════════════════════════════════

TEST(pitcher_passthrough_at_zero_semitones) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(0.0);

    // Feed a 440Hz sine and check output contains energy near 440Hz
    float input[4800], output[4800];
    fillSine(input, 4800, 440.0, 48000.0);
    for (int i = 0; i < 4800; ++i)
        output[i] = float(ps.process(double(input[i])));

    // RMS should be similar (within 50% — grains cause some amplitude variation)
    double inRms = rms(input, 4800);
    double outRms = rms(output + 1440, 3360);  // skip first grain startup
    EXPECT(outRms > inRms * 0.3 && outRms < inRms * 2.0);
}

TEST(pitcher_octave_up) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(12.0);

    float output[48000];
    for (int i = 0; i < 48000; ++i) {
        double input = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
        output[i] = float(ps.process(input));
    }

    // Check that 880Hz content is present (Goertzel)
    double mag440 = goertzel(output + 4800, 43200, 440.0, 48000.0);
    double mag880 = goertzel(output + 4800, 43200, 880.0, 48000.0);
    EXPECT(mag880 > mag440 * 0.5);  // 880Hz should be prominent
}

TEST(pitcher_no_invalid_samples) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(7.0);  // up a fifth

    float output[48000];
    for (int i = 0; i < 48000; ++i) {
        double input = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
        output[i] = float(ps.process(input));
    }
    EXPECT(!hasInvalidSamples(output, 48000));
}

TEST(pitcher_no_runaway) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(24.0);  // +2 octaves — extreme

    float output[48000];
    for (int i = 0; i < 48000; ++i) {
        double input = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
        output[i] = float(ps.process(input));
    }
    EXPECT(peakAbs(output, 48000) < 5.0);
}

TEST(pitcher_negative_semitones) {
    const int N = 48000;
    double buf[N];
    GranularPitchShifter ps;
    ps.configure(buf, N, 48000.0, 30.0);
    ps.setPitchSemitones(-12.0);  // octave down

    float output[48000];
    for (int i = 0; i < 48000; ++i) {
        double input = 0.5 * std::sin(2.0 * M_PI * 880.0 * i / 48000.0);
        output[i] = float(ps.process(input));
    }
    // Should have 440Hz content
    double mag440 = goertzel(output + 4800, 43200, 440.0, 48000.0);
    double mag880 = goertzel(output + 4800, 43200, 880.0, 48000.0);
    EXPECT(mag440 > mag880 * 0.3);  // 440Hz should have significant energy
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

    // With no automation (0 semitones shift), output should have energy
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

    // Note on: C4 (60)
    p.handleMIDIEvent(0x90, 60, 100);
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) != 0);

    // Note off
    p.handleMIDIEvent(0x80, 60, 0);
    EXPECT((p.activeNoteBitmaskLo() & (1ULL << 60)) == 0);
}

TEST(processor_midi_note_on_velocity_zero_is_off) {
    GlideProcessor p;
    p.setUp(2, 48000.0);

    p.handleMIDIEvent(0x90, 64, 100);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 0)) != 0);  // note 64 is bit 0 of hi

    // Note on with velocity 0 = note off
    p.handleMIDIEvent(0x90, 64, 0);
    EXPECT((p.activeNoteBitmaskHi() & (1ULL << 0)) == 0);
}

TEST(processor_parameter_set_and_get) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kGlideTime, 500.0f);
    p.setParameter(kMix, 75.0f);

    StereoBuffer buf;
    p.process(buf.channels, 2, 48000);  // let smoothers converge

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
    p.process(buf.channels, 2, 48000);  // 1 second

    // At 120 BPM, 1 second = 2 beats
    double pos = p.currentBeatPosition();
    EXPECT(std::fabs(pos - 2.0) < 0.1);
}

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
    printf("\nMyGlide DSP Tests\n");
    printf("=================\n\n");

    printf("AutomationCurve:\n");
    // Tests run via static initialization

    printf("\nResults: %d passed, %d failed\n\n", gTestsPassed, gTestsFailed);
    return gTestsFailed > 0 ? 1 : 0;
}
