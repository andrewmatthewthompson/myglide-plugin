# MyRythem

AUv3 **MIDI effect** plugin for Logic Pro (macOS 13+). Takes a held chord
and re-gates it in a rhythmic pattern — dotted 8ths, 8th+16th, triplets,
etc. Like an arpeggiator, but every hit plays the full chord stack instead
of unfolding note-by-note.

Status: **early scaffolding (v0.1)** — backend DSP is implemented and
tested, UI is a minimal placeholder.

## Concept

- Input MIDI chord → output re-gated MIDI on the rhythm pattern you pick.
- All held notes fire together on every active step.
- Tempo-synced to host, or free-running.
- Patterns include dotted 8ths, 8th+16th, triplets, shuffle, custom.

## Project Layout

```
MyRythem/                    Host container app (registers the AU)
MyRythemAU/                  AUv3 app extension (aumi)
  AudioUnitFactory.swift     Principal class for extension
  RythemAudioUnit.swift      AUAudioUnit subclass + render loop
  Parameters/
    RythemParameters.swift   AUParameterTree definitions
  UI/
    RythemMainView.swift     Placeholder SwiftUI view (step strip)
  DSP/
    RythemProcessor.hpp      Note tracker + step scheduler (core logic)
    NoteTracker.hpp          Fixed-capacity held-note set
    StepClock.hpp            Beat → step conversion + swing
    PatternBank.hpp          Built-in + custom patterns
    RythemDSPKernel.hpp      Thin wrapper
    RythemDSPKernelBridge.h  Obj-C interface (visible to Swift)
    RythemDSPKernelBridge.mm Obj-C++ bridge implementation
    MyRythemAU-Bridging-Header.h
Tests/
  test_rythem_dsp.cpp        Standalone C++ tests (no Xcode required)
```

## Parameters (v0.1)

| Addr | Name            | Range     | Unit    | Default |
|------|-----------------|-----------|---------|---------|
| 0    | Pattern         | 0–9       | indexed | 3 (8th+16th) |
| 1    | Rate            | 0–5       | indexed | 3 (1/16) |
| 2    | Gate            | 5–100     | %       | 60 |
| 3    | Swing           | 50–75     | %       | 50 |
| 4    | Velocity Mode   | 0–2       | indexed | 1 (FromInput) |
| 5    | Fixed Velocity  | 1–127     | —       | 100 |
| 6    | Accent          | 0–100     | %       | 30 |
| 7    | Octave          | -2..+2    | oct     | 0 |
| 8    | Latch           | 0–1       | bool    | 0 |
| 9    | Sync Mode       | 0–1       | indexed | 0 (Host) |
| 10   | Free BPM        | 30–300    | BPM     | 120 |
| 11   | Release Mode    | 0–1       | indexed | 0 (Immediate) |
| 12   | Pattern Length  | 1–16      | steps   | 16 |

## Patterns

0. Straight 8ths
1. Straight 16ths
2. Dotted 8ths (3/16)
3. 8th + 16th (♪ ♬ ♪ ♬)
4. 16th + 8th
5. Dotted 8th + 16th (3/16 + 1/16)
6. 8th Triplets
7. Shuffle 8ths
8. Quarter Pulse
9. Custom (bit-mask)

## Requirements

- macOS 13.0+
- Xcode 15+
- XcodeGen (`brew install xcodegen`)

## Building

```bash
cd MyRythem
./setup.sh                   # Generate MyRythem.xcodeproj
open MyRythem.xcodeproj      # Build in Xcode (Cmd+B)
```

Debug builds auto-install to `/Applications/MyRythem.app` and re-register
with LaunchServices.

## Using in Logic Pro

Insert on an instrument track: **MIDI FX → Audio Units → Demo → MyRythem**

Play a chord — MyRythem will re-gate it into the selected rhythm pattern.

## Verify Registration

```bash
auval -a | grep -i myrythem
# Expected: aumi Ryt1 Demo  -  MyRythem
```

## Tests (standalone, no Xcode needed)

```bash
c++ -std=c++17 -O2 -I MyRythemAU/DSP Tests/test_rythem_dsp.cpp -o Tests/test_rythem
./Tests/test_rythem
```

Covers: NoteTracker (insert/remove/dedupe/clear), PatternBank (step-active
lookups for all built-in patterns), StepClock (beat→step conversion,
swing offsets, transport sync), RythemProcessor (chord capture, step
scheduling, gate-off emission, octave transpose, latch).
