#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

@interface GlideDSPKernelBridge : NSObject

- (void)setUp:(int32_t)channelCount sampleRate:(double)sampleRate;
- (void)tearDown;
- (void)setParameter:(AUParameterAddress)address value:(AUValue)value;
- (AUValue)getParameter:(AUParameterAddress)address;
- (void)process:(AudioBufferList *)bufferList frameCount:(AUAudioFrameCount)frameCount;

// MIDI event handling
- (void)handleMIDIEvent:(uint8_t)status data1:(uint8_t)data1 data2:(uint8_t)data2;

// Musical context
- (void)setBeatPosition:(double)beatPosition tempo:(double)tempo;

// Automation curve manipulation (proper bridge — no raw pointers from Swift)
- (void)automationBeginEdit;
- (void)automationAddBreakpointAtBeat:(double)beat semitones:(double)semitones interpType:(uint8_t)interpType;
- (void)automationRemoveBreakpointAtIndex:(int)index;
- (void)automationMoveBreakpointAtIndex:(int)index toBeat:(double)beat semitones:(double)semitones;
- (void)automationClear;
- (void)automationCommitEdit;

// Active note bitmask (for UI display)
- (uint64_t)activeNoteBitmaskLo;
- (uint64_t)activeNoteBitmaskHi;
- (double)currentBeatPosition;

@end
