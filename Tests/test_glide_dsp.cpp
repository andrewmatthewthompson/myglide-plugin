#include "GlideProcessor.hpp"
#include <cstdio>
#include <cmath>
#include <cstring>

// ── Minimal test framework (same pattern as MyVerb) ──────────────────────

static int gTestsPassed = 0;
static int gTestsFailed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { \
            printf("  %-50s ", #name); \
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
    for (int i = 0; i < n; ++i) {
        if (std::isnan(buf[i]) || std::isinf(buf[i])) return true;
    }
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

struct StereoBuffer {
    static constexpr int kMaxFrames = 48000;  // 1 second @ 48kHz
    float left[kMaxFrames]  = {};
    float right[kMaxFrames] = {};
    float* channels[2] = { left, right };

    void clear() {
        std::memset(left, 0, sizeof(left));
        std::memset(right, 0, sizeof(right));
    }
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST(silence_in_silence_out) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    StereoBuffer buf;
    p.process(buf.channels, 2, 4800);
    EXPECT(rms(buf.left, 4800) < 1e-6 && rms(buf.right, 4800) < 1e-6);
}

TEST(passthrough_with_100_percent_mix) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 100.0f);
    StereoBuffer buf;
    // Fill with impulse
    buf.left[0] = 1.0f;
    buf.right[0] = 1.0f;
    p.process(buf.channels, 2, 4800);
    // With passthrough DSP, output should contain the impulse
    EXPECT(peakAbs(buf.left, 4800) > 0.5);
}

TEST(dry_signal_at_0_percent_mix) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kMix, 0.0f);
    StereoBuffer buf;
    buf.left[0] = 1.0f;
    // Process enough frames for smoother to converge
    p.process(buf.channels, 2, 4800);
    // At 0% mix, output = dry only (should still have the impulse)
    EXPECT(peakAbs(buf.left, 4800) > 0.5);
}

TEST(no_invalid_samples) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    StereoBuffer buf;
    buf.left[0] = 1.0f;
    buf.right[0] = -1.0f;
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000) && !hasInvalidSamples(buf.right, 48000));
}

TEST(extreme_parameters_no_crash) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    // Set extreme values
    p.setParameter(kGlideTime, 2000.0f);
    p.setParameter(kMix, 100.0f);
    StereoBuffer buf;
    buf.left[0] = 1.0f;
    p.process(buf.channels, 2, 48000);
    EXPECT(!hasInvalidSamples(buf.left, 48000));
}

TEST(parameter_set_and_get) {
    GlideProcessor p;
    p.setUp(2, 48000.0);
    p.setParameter(kGlideTime, 500.0f);
    // After smoothing converges, should be close to target
    StereoBuffer buf;
    p.process(buf.channels, 2, 48000);
    float val = p.getParameter(kGlideTime);
    EXPECT(std::fabs(val - 500.0f) < 1.0f);
}

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
    printf("\nMyGlide DSP Tests\n");
    printf("=================\n\n");
    printf("\nResults: %d passed, %d failed\n\n", gTestsPassed, gTestsFailed);
    return gTestsFailed > 0 ? 1 : 0;
}
