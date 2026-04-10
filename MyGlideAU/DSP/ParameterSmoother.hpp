#pragma once
#include <atomic>
#include <cmath>

/// Exponential parameter smoother — prevents zipper noise on automation.
/// Thread-safe: setTarget() from any thread, next() from audio thread only.
/// Proven pattern from MyVerb.
class ParameterSmoother {
public:
    ParameterSmoother() = default;

    void configure(double sampleRate, double convergenceMs = 7.0) {
        double samples = convergenceMs * 0.001 * sampleRate;
        mCoeff = (samples > 0.0) ? std::exp(-1.0 / samples) : 0.0;
        mCurrent = mTarget.load(std::memory_order_relaxed);
    }

    /// Update only the smoothing coefficient without resetting mCurrent.
    /// Use this when changing the convergence rate mid-stream (e.g. glide time knob)
    /// to avoid audible pitch discontinuities.
    void updateCoefficient(double sampleRate, double convergenceMs) {
        double samples = convergenceMs * 0.001 * sampleRate;
        mCoeff = (samples > 0.0) ? std::exp(-1.0 / samples) : 0.0;
    }

    void setTarget(double value) {
        mTarget.store(value, std::memory_order_relaxed);
    }

    double next() {
        double target = mTarget.load(std::memory_order_relaxed);
        mCurrent = mCurrent * mCoeff + target * (1.0 - mCoeff);
        return mCurrent;
    }

    double current() const { return mCurrent; }

    /// Advance N samples without producing output (fast skip).
    void advance(int32_t n) {
        double target = mTarget.load(std::memory_order_relaxed);
        double factor = std::pow(mCoeff, static_cast<double>(n));
        mCurrent = mCurrent * factor + target * (1.0 - factor);
    }

private:
    std::atomic<double> mTarget{0.0};
    double mCurrent = 0.0;
    double mCoeff   = 0.0;
};
