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

- (void *)automationCurvePtr {
    return _kernel.automationCurvePtr();
}

- (uint64_t)activeNoteBitmaskLo {
    return _kernel.activeNoteBitmaskLo();
}

- (uint64_t)activeNoteBitmaskHi {
    return _kernel.activeNoteBitmaskHi();
}

- (double)currentBeatPosition {
    return _kernel.currentBeatPosition();
}

@end
