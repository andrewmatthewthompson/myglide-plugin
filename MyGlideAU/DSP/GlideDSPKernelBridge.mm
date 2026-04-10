#import "GlideDSPKernelBridge.h"
#include "GlideDSPKernel.hpp"

@implementation GlideDSPKernelBridge {
    GlideDSPKernel _kernel;
}

- (void)setUp:(int32_t)channelCount sampleRate:(double)sampleRate {
    _kernel.setUp(channelCount, sampleRate);
}

- (void)tearDown {
    _kernel.tearDown();
}

- (void)setParameter:(AUParameterAddress)address value:(AUValue)value {
    _kernel.setParameter(address, value);
}

- (AUValue)getParameter:(AUParameterAddress)address {
    return _kernel.getParameter(address);
}

- (void)process:(AudioBufferList *)bufferList frameCount:(AUAudioFrameCount)frameCount {
    _kernel.process(bufferList, frameCount);
}

- (void)handleMIDIEvent:(uint8_t)status data1:(uint8_t)data1 data2:(uint8_t)data2 {
    _kernel.handleMIDIEvent(status, data1, data2);
}

- (void)setBeatPosition:(double)beatPosition tempo:(double)tempo {
    _kernel.setBeatPosition(beatPosition, tempo);
}

// ── Automation curve editing ─────────────────────────────────────────────

- (void)automationBeginEdit { _kernel.automationBeginEdit(); }

- (void)automationAddBreakpointAtBeat:(double)beat semitones:(double)semitones interpType:(uint8_t)interpType {
    _kernel.automationAddBreakpoint(beat, semitones, interpType);
}

- (void)automationRemoveBreakpointAtIndex:(int)index { _kernel.automationRemoveBreakpoint(index); }

- (void)automationMoveBreakpointAtIndex:(int)index toBeat:(double)beat semitones:(double)semitones {
    _kernel.automationMoveBreakpoint(index, beat, semitones);
}

- (void)automationClear { _kernel.automationClear(); }
- (void)automationCommitEdit { _kernel.automationCommitEdit(); }

// ── Automation serialization ─────────────────────────────────────────────

- (NSData *)automationSerialize {
    uint8_t buf[4 + 256 * 24];  // max: header + 256 breakpoints
    int len = _kernel.automationSerialize(buf, sizeof(buf));
    if (len <= 0) return [NSData data];
    return [NSData dataWithBytes:buf length:len];
}

- (void)automationDeserializeFromData:(NSData *)data {
    if (!data || data.length == 0) return;
    _kernel.automationDeserialize(static_cast<const uint8_t *>(data.bytes), static_cast<int>(data.length));
}

- (int)automationBreakpointCount {
    return _kernel.automationBreakpointCount();
}

- (void)automationBreakpointAtIndex:(int)index beat:(double *)beat semitones:(double *)semitones interpType:(uint8_t *)interpType {
    _kernel.automationBreakpointAt(index, beat, semitones, interpType);
}

// ── Display state ────────────────────────────────────────────────────────

- (uint64_t)activeNoteBitmaskLo { return _kernel.activeNoteBitmaskLo(); }
- (uint64_t)activeNoteBitmaskHi { return _kernel.activeNoteBitmaskHi(); }
- (double)currentBeatPosition { return _kernel.currentBeatPosition(); }
- (double)currentPitchSemitones { return _kernel.currentPitchSemitones(); }

- (int32_t)latencySamples { return _kernel.latencySamples(); }
- (double)tailTimeSeconds { return _kernel.tailTimeSeconds(); }

@end
