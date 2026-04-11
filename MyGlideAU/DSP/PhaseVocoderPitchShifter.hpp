#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

/// STFT-based phase vocoder pitch shifter.
///
/// Higher quality than granular for vocals/melodic content at the cost of
/// more latency (FFT size = 2048 samples ≈ 43ms at 48kHz) and more CPU.
///
/// Algorithm:
///   1. Accumulate input in a ring buffer
///   2. Every hop (FFT/4 = 512 samples), run one STFT frame:
///      a. Window input → FFT → polar → compute instantaneous frequency
///      b. Shift bins by pitch ratio → accumulate synthesis phase
///      c. Polar → Cartesian → IFFT → window → overlap-add to output ring
///   3. Read output from the output ring buffer (one sample per process() call)
///
/// Memory model: all buffers via external pointers (RT-safe, no allocations
/// in process). The parent GlideProcessor allocates a contiguous block.
///
/// Interface matches GranularPitchShifter so they can be swapped by mode.
class PhaseVocoderPitchShifter {
public:
    static constexpr int32_t kFFTSize = 2048;
    static constexpr int32_t kHopSize = kFFTSize / 4;  // 512
    static constexpr int32_t kHalfFFT = kFFTSize / 2 + 1;  // 1025

    /// Total doubles needed per channel from the contiguous memory block.
    static constexpr int32_t kMemoryPerChannel =
        kFFTSize +          // input ring
        kFFTSize * 2 +      // output ring (double-buffered for OLA)
        kFFTSize +          // FFT work real
        kFFTSize +          // FFT work imag
        kHalfFFT +          // previous phase
        kHalfFFT +          // synthesis phase
        kHalfFFT +          // magnitude workspace
        kHalfFFT;           // frequency workspace
    // = 2048 + 4096 + 2048 + 2048 + 1025*4 = 14340

    PhaseVocoderPitchShifter() = default;

    // Non-copyable: holds non-owning pointers to shared memory.
    PhaseVocoderPitchShifter(const PhaseVocoderPitchShifter&) = delete;
    PhaseVocoderPitchShifter& operator=(const PhaseVocoderPitchShifter&) = delete;

    /// Configure with external memory block and shared Hann window.
    /// bufferPtr must point to at least kMemoryPerChannel doubles.
    /// hannPtr must point to kFFTSize doubles (precomputed Hann window).
    void configure(double* bufferPtr, int32_t bufferSize,
                   double* hannPtr, int32_t /* grainSamples = kFFTSize */,
                   double sampleRate) {
        mSampleRate = sampleRate;
        mHannWindow = hannPtr;

        // Carve up the contiguous block
        double* ptr = bufferPtr;
        mInputRing   = ptr; ptr += kFFTSize;
        mOutputRing  = ptr; ptr += kFFTSize * 2;
        mFFTWorkReal = ptr; ptr += kFFTSize;
        mFFTWorkImag = ptr; ptr += kFFTSize;
        mPrevPhase   = ptr; ptr += kHalfFFT;
        mSynthPhase  = ptr; ptr += kHalfFFT;
        mMagnitude   = ptr; ptr += kHalfFFT;
        mFrequency   = ptr;

        // Zero everything
        std::memset(bufferPtr, 0, sizeof(double) * std::min(bufferSize, kMemoryPerChannel));

        mInputWritePos = 0;
        mOutputReadPos = 0;
        mInputCount = 0;
        mPitchRatio = 1.0;
        mLastSemitones = 0.0;
    }

    /// Set pitch shift in semitones. Clamped to ±48.
    void setPitchSemitones(double semitones) {
        if (semitones > 48.0) semitones = 48.0;
        if (semitones < -48.0) semitones = -48.0;
        if (std::fabs(semitones - mLastSemitones) > 0.001) {
            mPitchRatio = std::pow(2.0, semitones / 12.0);
            mLastSemitones = semitones;
        }
    }

    /// Process a single sample. Write input, read pitch-shifted output.
    double process(double input) {
        if (!mInputRing) return input;

        // Write to input ring
        mInputRing[mInputWritePos] = input;
        mInputWritePos = (mInputWritePos + 1) % kFFTSize;

        // Read from output ring
        double output = mOutputRing[mOutputReadPos];
        mOutputRing[mOutputReadPos] = 0.0;  // clear for next OLA cycle
        mOutputReadPos = (mOutputReadPos + 1) % (kFFTSize * 2);

        // Trigger STFT frame every hop
        mInputCount++;
        if (mInputCount >= kHopSize) {
            mInputCount = 0;
            processFrame();
        }

        return output;
    }

    double pitchRatio() const { return mPitchRatio; }

private:
    // ── STFT frame processing ────────────────────────────────────────────

    void processFrame() {
        const double twoPi = 2.0 * M_PI;
        const double freqPerBin = mSampleRate / static_cast<double>(kFFTSize);
        const double expectedPhaseDiff = twoPi * static_cast<double>(kHopSize) / static_cast<double>(kFFTSize);

        // === ANALYSIS ===

        // 1. Window the most recent FFT_SIZE samples from the input ring
        for (int32_t i = 0; i < kFFTSize; ++i) {
            int32_t readIdx = (mInputWritePos - kFFTSize + i + kFFTSize) % kFFTSize;
            mFFTWorkReal[i] = mInputRing[readIdx] * mHannWindow[i];
            mFFTWorkImag[i] = 0.0;
        }

        // 2. Forward FFT
        fft(mFFTWorkReal, mFFTWorkImag, kFFTSize, false);

        // 3. Convert to polar, compute instantaneous frequency
        for (int32_t k = 0; k < kHalfFFT; ++k) {
            double real = mFFTWorkReal[k];
            double imag = mFFTWorkImag[k];
            double mag = std::sqrt(real * real + imag * imag);
            double phase = std::atan2(imag, real);

            // Phase difference from previous frame
            double phaseDiff = phase - mPrevPhase[k];
            mPrevPhase[k] = phase;

            // Remove expected phase advance for this bin
            phaseDiff -= static_cast<double>(k) * expectedPhaseDiff;

            // Wrap to [-pi, pi]
            phaseDiff -= twoPi * std::floor(phaseDiff / twoPi + 0.5);

            // True frequency
            double trueFreq = static_cast<double>(k) * freqPerBin +
                               phaseDiff * freqPerBin / expectedPhaseDiff;

            mMagnitude[k] = mag;
            mFrequency[k] = trueFreq;
        }

        // === MODIFICATION: shift bins by pitch ratio ===

        // Reuse FFTWork as shifted mag/freq (we're done with the Cartesian data)
        double* shiftedMag  = mFFTWorkReal;
        double* shiftedFreq = mFFTWorkImag;
        std::memset(shiftedMag, 0, sizeof(double) * kHalfFFT);
        std::memset(shiftedFreq, 0, sizeof(double) * kHalfFFT);

        for (int32_t k = 0; k < kHalfFFT; ++k) {
            int32_t newBin = static_cast<int32_t>(std::round(static_cast<double>(k) * mPitchRatio));
            if (newBin >= 0 && newBin < kHalfFFT) {
                shiftedMag[newBin] += mMagnitude[k];
                shiftedFreq[newBin] = mFrequency[k] * mPitchRatio;
            }
        }

        // === SYNTHESIS ===

        // 5. Accumulate synthesis phase
        for (int32_t k = 0; k < kHalfFFT; ++k) {
            double phaseInc = twoPi * shiftedFreq[k] / mSampleRate * static_cast<double>(kHopSize);
            mSynthPhase[k] += phaseInc;
        }

        // 6. Convert back to Cartesian
        for (int32_t k = 0; k < kHalfFFT; ++k) {
            mFFTWorkReal[k] = shiftedMag[k] * std::cos(mSynthPhase[k]);
            mFFTWorkImag[k] = shiftedMag[k] * std::sin(mSynthPhase[k]);
        }

        // 7. Mirror for negative frequencies (conjugate symmetry)
        for (int32_t k = 1; k < kFFTSize / 2; ++k) {
            mFFTWorkReal[kFFTSize - k] =  mFFTWorkReal[k];
            mFFTWorkImag[kFFTSize - k] = -mFFTWorkImag[k];
        }

        // 8. Inverse FFT
        fft(mFFTWorkReal, mFFTWorkImag, kFFTSize, true);

        // 9. Window output (for perfect reconstruction with OLA)
        for (int32_t i = 0; i < kFFTSize; ++i) {
            mFFTWorkReal[i] *= mHannWindow[i];
        }

        // 10. Overlap-add to output ring
        // Normalization: Hann² at 75% overlap sums to 1.5, so scale by 2/3
        const double olaScale = 2.0 / 3.0;
        for (int32_t i = 0; i < kFFTSize; ++i) {
            int32_t outIdx = (mOutputReadPos + i) % (kFFTSize * 2);
            mOutputRing[outIdx] += mFFTWorkReal[i] * olaScale;
        }
    }

public:
    // ── In-place radix-2 Cooley-Tukey FFT (public for testing) ───────────

    static void fft(double* real, double* imag, int32_t n, bool inverse) {
        bitReverse(real, imag, n);

        for (int32_t stage = 2; stage <= n; stage *= 2) {
            int32_t halfStage = stage / 2;
            double angleStep = (inverse ? 1.0 : -1.0) * 2.0 * M_PI / static_cast<double>(stage);

            for (int32_t group = 0; group < n; group += stage) {
                for (int32_t k = 0; k < halfStage; ++k) {
                    double angle = angleStep * static_cast<double>(k);
                    double twR = std::cos(angle);
                    double twI = std::sin(angle);

                    int32_t even = group + k;
                    int32_t odd  = group + k + halfStage;

                    double tmpR = twR * real[odd] - twI * imag[odd];
                    double tmpI = twR * imag[odd] + twI * real[odd];

                    real[odd]  = real[even] - tmpR;
                    imag[odd]  = imag[even] - tmpI;
                    real[even] += tmpR;
                    imag[even] += tmpI;
                }
            }
        }

        if (inverse) {
            double inv = 1.0 / static_cast<double>(n);
            for (int32_t i = 0; i < n; ++i) {
                real[i] *= inv;
                imag[i] *= inv;
            }
        }
    }

    static void bitReverse(double* real, double* imag, int32_t n) {
        int32_t j = 0;
        for (int32_t i = 0; i < n; ++i) {
            if (i < j) {
                std::swap(real[i], real[j]);
                std::swap(imag[i], imag[j]);
            }
            int32_t m = n >> 1;
            while (m >= 1 && j >= m) {
                j -= m;
                m >>= 1;
            }
            j += m;
        }
    }

private:
    // ── State ────────────────────────────────────────────────────────────

    double* mInputRing    = nullptr;
    double* mOutputRing   = nullptr;
    double* mFFTWorkReal  = nullptr;
    double* mFFTWorkImag  = nullptr;
    double* mPrevPhase    = nullptr;
    double* mSynthPhase   = nullptr;
    double* mMagnitude    = nullptr;
    double* mFrequency    = nullptr;
    double* mHannWindow   = nullptr;

    int32_t mInputWritePos = 0;
    int32_t mOutputReadPos = 0;
    int32_t mInputCount    = 0;

    double  mPitchRatio    = 1.0;
    double  mLastSemitones = 0.0;
    double  mSampleRate    = 48000.0;
};
