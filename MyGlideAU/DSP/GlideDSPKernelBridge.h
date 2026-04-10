#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

@interface GlideDSPKernelBridge : NSObject

- (void)setUp:(int32_t)channelCount sampleRate:(double)sampleRate;
- (void)tearDown;
- (void)setParameter:(AUParameterAddress)address value:(AUValue)value;
- (AUValue)getParameter:(AUParameterAddress)address;
- (void)process:(AudioBufferList *)bufferList frameCount:(AUAudioFrameCount)frameCount;

@end
