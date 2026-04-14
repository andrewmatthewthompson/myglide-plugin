#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

@interface RythemDSPKernelBridge : NSObject

- (void)setUp:(int32_t)channelCount sampleRate:(double)sampleRate;
- (void)tearDown;

- (void)setParameter:(AUParameterAddress)address value:(AUValue)value;
- (AUValue)getParameter:(AUParameterAddress)address;

- (void)setCustomPatternMask:(uint32_t)mask;
- (uint32_t)customPatternMask;

- (void)handleMIDIEvent:(uint8_t)status data1:(uint8_t)data1 data2:(uint8_t)data2 frameOffset:(uint32_t)frameOffset;
- (void)setBeatPosition:(double)beatPosition tempo:(double)tempo;
- (void)onTransportJump;

- (void)process:(AUAudioFrameCount)frameCount;

// Output queue readback (for the render block to emit via MIDIOutputEventBlock).
- (int)pendingMIDIEventCount;
- (void)pendingMIDIEventAtIndex:(int)index
                         status:(uint8_t *)status
                          data1:(uint8_t *)data1
                          data2:(uint8_t *)data2
                    frameOffset:(uint32_t *)frameOffset;

- (void)killAllSounding:(uint32_t)frameOffset;

// Display state
- (int)heldNoteCount;
- (int)latchedNoteCount;
- (int)currentStepIndex;
- (BOOL)stepIsActiveNow;

@end
