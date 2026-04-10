# MyGlide

AUv3 glide/portamento effect plugin for Logic Pro (macOS 13+).

## Architecture

Same proven stack as [MyVerb](https://github.com/andrewmatthewthompson/logic-pro-reverb):

- **Swift** — UI (SwiftUI) + AU framework (`AUAudioUnit` subclass)
- **C++17** — DSP engine (double-precision internal, RT-safe)
- **Obj-C++** — Bridge layer (Swift ↔ C++)
- **XcodeGen** — Project generation from `project.yml`

## Project Layout

```
MyGlide/                    Host container app (registers the AU)
MyGlideAU/                  AUv3 app extension
  AudioUnitFactory.swift    Principal class for extension
  GlideAudioUnit.swift      AUAudioUnit subclass + render loop
  Parameters/
    GlideParameters.swift   AUParameterTree definitions
  UI/
    GlideMainView.swift     SwiftUI plugin panel + knob controls
  DSP/
    GlideProcessor.hpp      Main DSP engine (TODO: implement glide)
    GlideDSPKernel.hpp      Thin wrapper (AudioBufferList → float**)
    GlideDSPKernelBridge.h  Obj-C interface (visible to Swift)
    GlideDSPKernelBridge.mm Obj-C++ bridge implementation
    ParameterSmoother.hpp   Exponential smoothing (no zipper noise)
Tests/
  test_glide_dsp.cpp        Standalone C++ DSP tests
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

## Verify Registration

```bash
auval -a | grep -i myglide
# Expected: aufx Gld1 Demo  -  MyGlide

pluginkit -mAvvv 2>&1 | grep com.myglide
```

## Running Tests

```bash
c++ -std=c++17 -O2 -I MyGlideAU/DSP Tests/test_glide_dsp.cpp -o Tests/test_glide -lm
./Tests/test_glide
```

## Parameters

| Addr | Name       | Range     | Unit | Default |
|------|------------|-----------|------|---------|
| 0    | Glide Time | 1–2000    | ms   | 200     |
| 1    | Mix        | 0–100     | %    | 100     |

## In Logic Pro

Audio FX → Audio Units → Demo → MyGlide
