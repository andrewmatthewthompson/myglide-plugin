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

@end
