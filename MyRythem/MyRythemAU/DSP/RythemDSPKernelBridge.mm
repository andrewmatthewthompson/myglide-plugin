#import "RythemDSPKernelBridge.h"
#include "RythemDSPKernel.hpp"

@implementation RythemDSPKernelBridge {
    RythemDSPKernel _kernel;
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

- (void)setCustomPatternMask:(uint32_t)mask {
    _kernel.setCustomPatternMask(mask);
}

- (uint32_t)customPatternMask {
    return _kernel.customPatternMask();
}

- (void)handleMIDIEvent:(uint8_t)status data1:(uint8_t)data1 data2:(uint8_t)data2 frameOffset:(uint32_t)frameOffset {
    _kernel.handleMIDIEvent(status, data1, data2, frameOffset);
}

- (void)setBeatPosition:(double)beatPosition tempo:(double)tempo {
    _kernel.setBeatPosition(beatPosition, tempo);
}

- (void)onTransportJump {
    _kernel.onTransportJump();
}

- (void)process:(AUAudioFrameCount)frameCount {
    _kernel.process(frameCount);
}

- (int)pendingMIDIEventCount {
    return _kernel.pendingMIDIEventCount();
}

- (void)pendingMIDIEventAtIndex:(int)index
                         status:(uint8_t *)status
                          data1:(uint8_t *)data1
                          data2:(uint8_t *)data2
                    frameOffset:(uint32_t *)frameOffset {
    _kernel.pendingMIDIEventAt(index, status, data1, data2, frameOffset);
}

- (void)killAllSounding:(uint32_t)frameOffset {
    _kernel.killAllSounding(frameOffset);
}

- (int)heldNoteCount    { return _kernel.heldNoteCount(); }
- (int)latchedNoteCount { return _kernel.latchedNoteCount(); }
- (int)currentStepIndex { return _kernel.currentStepIndex(); }
- (BOOL)stepIsActiveNow { return _kernel.stepIsActiveNow() ? YES : NO; }

@end
