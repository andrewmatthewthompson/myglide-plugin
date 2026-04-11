# MyGlide

AUv3 MIDI pitch automation / glide plugin for Logic Pro (macOS 13+).

Two modes: **Manual** — draw pitch automation curves with snap-to-note
breakpoints. **Auto** — MIDI notes directly drive the pitch with smooth
portamento glides. Choose between a granular shifter (low latency, good for
effects) or a phase vocoder (higher quality, better for vocals). Designed for
melodic pitch glides — synth leads sliding between notes, DJ risers, etc.

## Features

**DSP engine** (C++17, 64-bit internal processing)
- **Two pitch shifting modes:**
  - **Granular** — 2 overlapping grains, Hann window, cubic Hermite. 30ms latency, 0.12% CPU
  - **Phase Vocoder** — STFT-based (FFT 2048, 75% overlap). 43ms latency, 1.7% CPU. More transparent on vocals
- Automation curve with triple-buffered lock-free breakpoint system
- 3 interpolation modes: Linear, Smooth (smoothstep), Step
- Parameter smoothing on all controls (no zipper noise)
- Anti-aliasing lowpass filter adapting to pitch ratio
- ±48 semitone range, denormal guarding, RT-safe
- Custom in-place radix-2 FFT (no Accelerate dependency — tests compile standalone)
- **MIDI-driven auto-glide** (last-note priority, highest-note fallback)
- Input/output peak level tracking (atomic, once per block)
- 407 test assertions passing

**Plugin**
- AUv3 music effect (`aumf`) — receives both MIDI and audio
- MIDI note tracking with active note display on piano roll
- Host transport sync (beat position + tempo)
- **Latency reporting** for PDC (adapts when switching modes), **tail time** for bounce, **bypass** support
- **7 factory presets** (Octave Glide Up/Down, Slow Portamento, DJ Riser, Chromatic Walk, Wobble, Step Sequence)
- **Preset save/load** via fullState — breakpoints survive session reopen
- **MIDI-driven auto-glide mode** — play notes and pitch glides automatically between them
- **DAW pitch offset parameter** — Logic Pro can automate pitch from its own lane

**UI**
- **Resizable window** (min 500x350, preferred 700x500)
- **[Manual | Auto] glide mode picker** + **[Granular | Vocoder] quality picker** in controls bar
- Piano roll sidebar with active MIDI note highlighting
- Automation canvas: click to add, drag to move, double-click to delete breakpoints
- **Curve drawing mode** (pencil tool) — drag to create breakpoints along a path
- **Zoom** (pinch) and **scroll** (drag ruler) with adaptive grid density
- **Beat subdivision snapping** (1/4, 1/8, 1/16) on X-axis, semitone snap on Y-axis
- **Undo/redo** (50 levels, Cmd+Z / Cmd+Shift+Z, drag coalescing)
- **Real-time pitch indicator** (+X.X st) in controls bar + dot on curve at playhead
- **Input/output level meters** (stereo, green/gold/red, 30fps)
- **Breakpoint selection** + Delete key
- Playhead tracking at 30fps

## Parameters

| Addr | Name          | Range    | Unit    | Default | Notes |
|------|---------------|----------|---------|---------|-------|
| 0    | Glide Time    | 1–2000   | ms      | 50      | Pitch smoothing rate |
| 1    | Mix           | 0–100    | %       | 100     | Wet/dry |
| 2    | Pitch Range   | 12–24    | semi    | 24      | Display range |
| 3    | Pitch Offset  | -24–+24  | semi    | 0       | DAW-automatable offset |
| 4    | Shifter Mode  | 0–1      | indexed | 0       | 0=Granular, 1=Vocoder |
| 5    | Auto Glide    | 0–1      | bool    | 0       | 0=Manual, 1=Auto (MIDI-driven) |

## Project Layout

```
MyGlide/                    Host container app (registers the AU)
MyGlideAU/                  AUv3 app extension
  AudioUnitFactory.swift    Principal class for extension
  GlideAudioUnit.swift      AUAudioUnit subclass + render loop + presets
  Parameters/
    GlideParameters.swift   AUParameterTree definitions (6 params)
  UI/
    GlideMainView.swift     Piano roll + automation editor + controls
  DSP/
    GlideProcessor.hpp      Main DSP (MIDI, automation, mode dispatch)
    AutomationCurve.hpp     Triple-buffered breakpoint system
    GranularPitchShifter.hpp  2-grain pitch shifter with Hann LUT
    PhaseVocoderPitchShifter.hpp  STFT phase vocoder with custom FFT
    GlideDSPKernel.hpp      Thin wrapper (AudioBufferList → float**)
    GlideDSPKernelBridge.h  Obj-C interface (visible to Swift)
    GlideDSPKernelBridge.mm Obj-C++ bridge implementation
    ParameterSmoother.hpp   Exponential smoothing (no zipper noise)
Tests/
  test_glide_dsp.cpp        387 assertions: functional, regression, perf
```

## Requirements

- macOS 13.0+
- Xcode 15+
- XcodeGen (`brew install xcodegen`)

## Building

```bash
./setup.sh                  # Generate MyGlide.xcodeproj
open MyGlide.xcodeproj      # Build in Xcode (Cmd+B)
```

Debug builds auto-install to `/Applications/MyGlide.app` and re-register with LaunchServices.

## Using in Logic Pro

Insert on an instrument track: Audio FX → Audio Units → Demo → MyGlide

The plugin receives MIDI from the track and shows notes on the piano roll.

**Manual mode**: Draw pitch automation by clicking on the canvas. The audio
is pitch-shifted according to the automation curve.

**Auto mode**: Play MIDI notes and the pitch glides smoothly between them.
Glide speed is controlled by the Glide Time parameter. Last-note priority
with highest-note fallback on release.

Switch between Granular and Vocoder quality using the picker in the controls bar.

## Verify Registration

```bash
auval -a | grep -i myglide
# Expected: aumf Gld1 Demo  -  MyGlide
```

## Tests

```bash
c++ -std=c++17 -O2 -I MyGlideAU/DSP Tests/test_glide_dsp.cpp -o Tests/test_glide -lm
./Tests/test_glide
```

407 test assertions covering: AutomationCurve (breakpoints, interpolation,
triple-buffer, serialization, edge cases), GranularPitchShifter (frequency
accuracy via Goertzel, extreme values, tiny buffers), PhaseVocoderPitchShifter
(FFT roundtrip, octave shift, mode switching, latency reporting),
GlideProcessor (MIDI tracking, parameter smoothing, beat position, bypass,
automation integration, vocoder/granular dispatch), and enforced performance
regression floors (granular >200x RT, vocoder >50x RT, <5% CPU worst case,
inline memory <25KB, vocoder memory <512KB/channel).
