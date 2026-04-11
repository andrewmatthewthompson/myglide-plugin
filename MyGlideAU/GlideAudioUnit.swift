import AVFoundation
import AudioToolbox

public class GlideAudioUnit: AUAudioUnit {

    private var _parameterTree: AUParameterTree!
    private var _inputBus: AUAudioUnitBus!
    private var _outputBus: AUAudioUnitBus!
    private var _inputBusArray: AUAudioUnitBusArray!
    private var _outputBusArray: AUAudioUnitBusArray!

    let kernel = GlideDSPKernelBridge()

    /// Called after fullState is restored so the UI can rebuild its breakpoint array.
    var onStateRestored: (() -> Void)?

    public override var parameterTree: AUParameterTree? {
        get { return _parameterTree }
        set { _parameterTree = newValue }
    }

    public override var inputBusses: AUAudioUnitBusArray { return _inputBusArray }
    public override var outputBusses: AUAudioUnitBusArray { return _outputBusArray }
    public override var supportsUserPresets: Bool { return true }

    // MARK: - Latency & Tail Time

    public override var latency: TimeInterval {
        return TimeInterval(kernel.latencySamples()) / (_outputBus?.format.sampleRate ?? 48000.0)
    }

    public override var tailTime: TimeInterval {
        return kernel.tailTimeSeconds()
    }

    // MARK: - Bypass

    // Heap-allocated bypass flag: safe to capture in the render block closure.
    // Using UnsafeMutablePointer avoids the undefined behavior of capturing &storedProperty.
    private let _bypassFlag: UnsafeMutablePointer<Bool> = {
        let ptr = UnsafeMutablePointer<Bool>.allocate(capacity: 1)
        ptr.initialize(to: false)
        return ptr
    }()
    public override var shouldBypassEffect: Bool {
        get { return _bypassFlag.pointee }
        set { _bypassFlag.pointee = newValue }
    }

    deinit {
        _bypassFlag.deinitialize(count: 1)
        _bypassFlag.deallocate()
    }

    // MARK: - Init

    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {
        try super.init(componentDescription: componentDescription, options: options)

        let format = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
        _inputBus = try AUAudioUnitBus(format: format)
        _outputBus = try AUAudioUnitBus(format: format)
        _inputBusArray = AUAudioUnitBusArray(audioUnit: self, busType: .input, busses: [_inputBus])
        _outputBusArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [_outputBus])

        setupParameterTree()
    }

    private func setupParameterTree() {
        let params = GlideParameters.createParameters()
        _parameterTree = AUParameterTree.createTree(withChildren: params)

        _parameterTree.implementorValueObserver = { [weak self] param, value in
            self?.kernel.setParameter(param.address, value: value)
            // Notify host when shifter mode changes (latency changes with it)
            if param.address == GlideParameters.Address.shifterMode.rawValue {
                self?.willChangeValue(forKey: "latency")
                self?.didChangeValue(forKey: "latency")
            }
        }

        _parameterTree.implementorValueProvider = { [weak self] param in
            return self?.kernel.getParameter(param.address) ?? 0
        }

        _parameterTree.implementorStringFromValueCallback = { param, valuePtr in
            let value = valuePtr?.pointee ?? param.value
            switch param.address {
            case 0: return String(format: "%.0f ms", value)
            case 1: return String(format: "%.0f%%", value)
            case 2: return String(format: "\u{00B1}%.0f", value)
            case 3: return String(format: "%+.1f st", value)
            case 4: return value < 0.5 ? "Granular" : "Vocoder"
            case 5: return value > 0.5 ? "Auto" : "Manual"
            default: return String(format: "%.1f", value)
            }
        }
    }

    // MARK: - Factory Presets

    private static let factoryPresetData: [(name: String, glideTime: Float, mix: Float, breakpoints: [(beat: Double, semi: Double, interp: UInt8)])] = [
        (name: "Octave Glide Up",
         glideTime: 80, mix: 100,
         breakpoints: [(0.0, 0.0, 1), (4.0, 12.0, 1)]),

        (name: "Octave Glide Down",
         glideTime: 80, mix: 100,
         breakpoints: [(0.0, 0.0, 1), (4.0, -12.0, 1)]),

        (name: "Slow Portamento",
         glideTime: 300, mix: 100,
         breakpoints: [(0.0, 0.0, 1), (4.0, 7.0, 1), (8.0, 0.0, 1), (12.0, 5.0, 1), (16.0, 0.0, 1)]),

        (name: "DJ Riser",
         glideTime: 50, mix: 100,
         breakpoints: [(0.0, 0.0, 0), (16.0, 24.0, 0)]),

        (name: "Chromatic Walk",
         glideTime: 30, mix: 100,
         breakpoints: [(0.0, 0.0, 0), (2.0, 2.0, 0), (4.0, 4.0, 0), (6.0, 7.0, 0), (8.0, 12.0, 0)]),

        (name: "Wobble",
         glideTime: 20, mix: 80,
         breakpoints: [(0.0, 0.0, 1), (1.0, 2.0, 1), (2.0, -2.0, 1), (3.0, 2.0, 1), (4.0, -2.0, 1),
                       (5.0, 2.0, 1), (6.0, -2.0, 1), (7.0, 2.0, 1), (8.0, 0.0, 1)]),

        (name: "Step Sequence",
         glideTime: 10, mix: 100,
         breakpoints: [(0.0, 0.0, 2), (2.0, 5.0, 2), (4.0, 3.0, 2), (6.0, 7.0, 2),
                       (8.0, 0.0, 2), (10.0, 5.0, 2), (12.0, 12.0, 2), (14.0, 0.0, 2)]),
    ]

    public override var factoryPresets: [AUAudioUnitPreset]? {
        return GlideAudioUnit.factoryPresetData.enumerated().map { index, data in
            let preset = AUAudioUnitPreset()
            preset.number = index
            preset.name = data.name
            return preset
        }
    }

    private var _currentPreset: AUAudioUnitPreset?
    public override var currentPreset: AUAudioUnitPreset? {
        get { return _currentPreset }
        set {
            _currentPreset = newValue
            guard let preset = newValue, preset.number >= 0,
                  preset.number < GlideAudioUnit.factoryPresetData.count else { return }

            let data = GlideAudioUnit.factoryPresetData[preset.number]

            // Set parameters
            _parameterTree?.parameter(withAddress: 0)?.value = data.glideTime
            _parameterTree?.parameter(withAddress: 1)?.value = data.mix
            _parameterTree?.parameter(withAddress: 3)?.value = 0  // reset pitch offset

            // Set automation breakpoints
            kernel.automationBeginEdit()
            kernel.automationClear()
            for bp in data.breakpoints {
                kernel.automationAddBreakpoint(atBeat: bp.beat, semitones: bp.semi, interpType: bp.interp)
            }
            kernel.automationCommitEdit()

            DispatchQueue.main.async { [weak self] in
                self?.onStateRestored?()
            }
        }
    }

    // MARK: - Preset Persistence (fullState)

    private static let automationKey = "automationCurveData"

    public override var fullState: [String: Any]? {
        get {
            var state = super.fullState ?? [:]
            if let data = kernel.automationSerialize(), data.count > 0 {
                state[GlideAudioUnit.automationKey] = data
            }
            return state
        }
        set {
            super.fullState = newValue
            if let data = newValue?[GlideAudioUnit.automationKey] as? Data {
                kernel.automationDeserialize(from: data)
                DispatchQueue.main.async { [weak self] in
                    self?.onStateRestored?()
                }
            }
        }
    }

    // MARK: - Render Resources

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        let sampleRate = _outputBus.format.sampleRate
        kernel.setUp(Int32(outputBusses[0].format.channelCount), sampleRate: sampleRate)
    }

    public override func deallocateRenderResources() {
        super.deallocateRenderResources()
        kernel.tearDown()
    }

    // MARK: - Render Block

    public override var internalRenderBlock: AUInternalRenderBlock {
        let kernRef = Unmanaged.passUnretained(kernel)
        let musicalContext = self.musicalContextBlock
        let bypassRef = _bypassFlag

        return { actionFlags, timestamp, frameCount, outputBusNumber,
                 outputData, realtimeEventListHead, pullInputBlock in

            guard let pullInputBlock = pullInputBlock else {
                return kAudioUnitErr_NoConnection
            }

            var pullFlags = AudioUnitRenderActionFlags(rawValue: 0)
            let status = pullInputBlock(&pullFlags, timestamp, frameCount, 0, outputData)
            guard status == noErr else { return status }

            if bypassRef.pointee { return noErr }

            let kern = kernRef.takeUnretainedValue()

            if let musicalContext = musicalContext {
                var tempo: Double = 120.0
                var beatPosition: Double = 0.0
                var timeSigNumerator: Double = 0
                var timeSigDenominator: Int = 0
                var sampleOffsetToNextBeat: Int = 0
                var currentMeasureDownbeat: Double = 0

                if musicalContext(&tempo,
                                  &timeSigNumerator,
                                  &timeSigDenominator,
                                  &beatPosition,
                                  &sampleOffsetToNextBeat,
                                  &currentMeasureDownbeat) {
                    kern.setBeatPosition(beatPosition, tempo: tempo)
                }
            }

            var eventPtr = realtimeEventListHead
            while let event = eventPtr {
                if event.pointee.head.eventType == .MIDI {
                    let midi = event.pointee.MIDI
                    kern.handleMIDIEvent(midi.data.0, data1: midi.data.1, data2: midi.data.2)
                }
                eventPtr = event.pointee.head.next
            }

            kern.process(outputData, frameCount: frameCount)
            return noErr
        }
    }
}
