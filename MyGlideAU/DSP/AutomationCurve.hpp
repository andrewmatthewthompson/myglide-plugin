#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

/// Interpolation type between breakpoints.
enum class InterpolationType : uint8_t {
    Linear = 0,
    Smooth = 1,   // Hermite / S-curve
    Step   = 2,
};

/// A single automation breakpoint: position in beats + pitch in semitones.
/// Explicitly sized for cross-language serialization (Swift reads this layout).
struct Breakpoint {
    double beat      = 0.0;
    double semitones = 0.0;   // relative pitch shift, ±24
    InterpolationType interp = InterpolationType::Linear;
    uint8_t _pad[7] = {};     // explicit padding to 24 bytes
};
static_assert(sizeof(Breakpoint) == 24, "Breakpoint must be 24 bytes for serialization");

/// Sorted breakpoint array with evaluation and triple-buffer for lock-free
/// thread safety between UI (writer) and audio thread (reader).
///
/// Thread contract:
///   Audio thread: swapIfPending(), evaluate(), getBreakpoints()
///   UI thread:    beginEdit(), add/remove/move/clear, commitEdit()
///   No field is written by both threads.
class AutomationCurve {
public:
    static constexpr int kMaxBreakpoints = 256;

    AutomationCurve() {
        for (int i = 0; i < 3; ++i) {
            mBuffers[i].count = 0;
        }
        mReadIndex.store(0, std::memory_order_relaxed);
        mPendingIndex.store(-1, std::memory_order_relaxed);
        mWriteIndex = 1;
    }

    // Non-copyable, non-movable (contains atomics + complex state)
    AutomationCurve(const AutomationCurve&) = delete;
    AutomationCurve& operator=(const AutomationCurve&) = delete;

    // ── Audio thread ─────────────────────────────────────────────────────

    /// Call at the top of each render block. Lock-free.
    void swapIfPending() {
        int pending = mPendingIndex.load(std::memory_order_acquire);
        if (pending >= 0) {
            // Publish the pending buffer as the new read buffer.
            // The old read buffer is now free — UI will discover this in beginEdit().
            mReadIndex.store(pending, std::memory_order_release);
            mPendingIndex.store(-1, std::memory_order_relaxed);
            mCachedSegment = -1;  // invalidate cache — new buffer may have different segments
            // NOTE: mWriteIndex is NOT touched here. Only UI thread writes it.
        }
    }

    /// Evaluate the automation curve at a given beat position.
    /// Returns pitch shift in semitones.
    /// Uses a segment cache to skip binary search during sequential playback.
    double evaluate(double beat) const {
        const Buffer& buf = mBuffers[mReadIndex.load(std::memory_order_acquire)];
        if (buf.count == 0) return 0.0;
        if (buf.count == 1) return buf.points[0].semitones;

        // Before first breakpoint: hold first value
        if (beat <= buf.points[0].beat) return buf.points[0].semitones;

        // After last breakpoint: hold last value
        if (beat >= buf.points[buf.count - 1].beat) return buf.points[buf.count - 1].semitones;

        // Fast path: check if we're still in the cached segment
        int lo = mCachedSegment;
        int hi = lo + 1;
        if (lo >= 0 && hi < buf.count &&
            beat >= buf.points[lo].beat && beat < buf.points[hi].beat) {
            // Cache hit — skip binary search
        } else {
            // Binary search for the segment containing beat
            lo = 0;
            hi = buf.count - 1;
            while (lo < hi - 1) {
                int mid = (lo + hi) / 2;
                if (buf.points[mid].beat <= beat) lo = mid;
                else hi = mid;
            }
            mCachedSegment = lo;
        }

        const Breakpoint& a = buf.points[lo];
        const Breakpoint& b = buf.points[hi];
        double span = b.beat - a.beat;
        if (span < 1e-9) return a.semitones;

        double t = (beat - a.beat) / span;

        switch (a.interp) {
            case InterpolationType::Step:
                return a.semitones;

            case InterpolationType::Smooth: {
                // Hermite S-curve: smoothstep
                double s = t * t * (3.0 - 2.0 * t);
                return a.semitones + (b.semitones - a.semitones) * s;
            }

            case InterpolationType::Linear:
            default:
                return a.semitones + (b.semitones - a.semitones) * t;
        }
    }

    /// Get read-only access to current breakpoints (audio thread).
    int getBreakpoints(const Breakpoint** out) const {
        const Buffer& buf = mBuffers[mReadIndex.load(std::memory_order_acquire)];
        *out = buf.points;
        return buf.count;
    }

    // ── UI thread ────────────────────────────────────────────────────────

    /// Begin an edit session: picks the free write buffer and copies current
    /// read buffer contents into it.
    void beginEdit() {
        // Find a buffer that is not the current read and not pending.
        int read = mReadIndex.load(std::memory_order_acquire);
        int pending = mPendingIndex.load(std::memory_order_acquire);
        for (int i = 0; i < 3; ++i) {
            if (i != read && i != pending) { mWriteIndex = i; break; }
        }

        const Buffer& src = mBuffers[read];
        Buffer& dst = mBuffers[mWriteIndex];
        dst.count = src.count;
        std::memcpy(dst.points, src.points, sizeof(Breakpoint) * src.count);
    }

    /// Add a breakpoint (inserts sorted by beat). Returns index, or -1 if full.
    int addBreakpoint(double beat, double semitones, InterpolationType interp = InterpolationType::Linear) {
        Buffer& buf = mBuffers[mWriteIndex];
        if (buf.count >= kMaxBreakpoints) return -1;

        // Find insertion point (sorted by beat)
        int idx = 0;
        while (idx < buf.count && buf.points[idx].beat < beat) ++idx;

        // Shift right
        for (int i = buf.count; i > idx; --i) {
            buf.points[i] = buf.points[i - 1];
        }

        buf.points[idx] = { beat, semitones, interp, {} };
        buf.count++;
        return idx;
    }

    /// Remove breakpoint at index.
    void removeBreakpoint(int index) {
        Buffer& buf = mBuffers[mWriteIndex];
        if (index < 0 || index >= buf.count) return;
        for (int i = index; i < buf.count - 1; ++i) {
            buf.points[i] = buf.points[i + 1];
        }
        buf.count--;
    }

    /// Move breakpoint at index to new position. Re-sorts.
    void moveBreakpoint(int index, double beat, double semitones) {
        Buffer& buf = mBuffers[mWriteIndex];
        if (index < 0 || index >= buf.count) return;
        InterpolationType interp = buf.points[index].interp;
        removeBreakpoint(index);
        addBreakpoint(beat, semitones, interp);
    }

    /// Set interpolation type for breakpoint at index.
    void setInterpolationType(int index, InterpolationType type) {
        Buffer& buf = mBuffers[mWriteIndex];
        if (index >= 0 && index < buf.count) {
            buf.points[index].interp = type;
        }
    }

    /// Clear all breakpoints in the write buffer.
    void clearBreakpoints() {
        mBuffers[mWriteIndex].count = 0;
    }

    /// Commit the edit: makes write buffer available to audio thread.
    void commitEdit() {
        mPendingIndex.store(mWriteIndex, std::memory_order_release);
        // mWriteIndex will be re-determined in the next beginEdit() call.
    }

    /// Get write buffer breakpoints for UI display. (UI thread only.)
    int getEditBreakpoints(const Breakpoint** out) const {
        const Buffer& buf = mBuffers[mWriteIndex];
        *out = buf.points;
        return buf.count;
    }

    /// Get current breakpoint count (for UI).
    int count() const {
        int pending = mPendingIndex.load(std::memory_order_acquire);
        if (pending >= 0) return mBuffers[pending].count;
        return mBuffers[mReadIndex.load(std::memory_order_acquire)].count;
    }

    // ── Serialization ────────────────────────────────────────────────────

    /// Serialize breakpoints to a byte buffer. Returns bytes written.
    int serialize(uint8_t* out, int maxBytes) const {
        const Buffer& buf = mBuffers[mReadIndex.load(std::memory_order_acquire)];
        int needed = 4 + buf.count * static_cast<int>(sizeof(Breakpoint));
        if (needed > maxBytes) return 0;

        int32_t cnt = buf.count;
        std::memcpy(out, &cnt, 4);
        std::memcpy(out + 4, buf.points, buf.count * sizeof(Breakpoint));
        return needed;
    }

    /// Deserialize breakpoints from byte buffer into the write buffer.
    /// Call beginEdit() first.
    void deserialize(const uint8_t* data, int length) {
        Buffer& buf = mBuffers[mWriteIndex];
        if (length < 4) { buf.count = 0; return; }

        int32_t cnt = 0;
        std::memcpy(&cnt, data, 4);
        if (cnt < 0) cnt = 0;
        if (cnt > kMaxBreakpoints) cnt = kMaxBreakpoints;

        int available = (length - 4) / static_cast<int>(sizeof(Breakpoint));
        if (cnt > available) cnt = available;

        std::memcpy(buf.points, data + 4, cnt * sizeof(Breakpoint));
        buf.count = cnt;
    }

private:
    struct Buffer {
        Breakpoint points[kMaxBreakpoints];
        int count = 0;
    };

    Buffer mBuffers[3];
    std::atomic<int> mReadIndex{0};      // written by audio thread only
    std::atomic<int> mPendingIndex{-1};  // written by UI (commit), read by audio (swap)
    int mWriteIndex = 1;                 // written by UI thread only
    mutable int mCachedSegment = -1;     // audio thread only: last binary search result
};
