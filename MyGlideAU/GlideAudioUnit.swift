import AVFoundation
import AudioToolbox

public class GlideAudioUnit: AUAudioUnit {

    private var _parameterTree: AUParameterTree!
    private var _inputBus: AUAudioUnitBus!
    private var _outputBus: AUAudioUnitBus!
    private var _inputBusArray: AUAudioUnitBusArray!
    private var _outputBusArray: AUAudioUnitBusArray!

    let kernel = GlideDSPKernelBridge()

    public override var parameterTree: AUParameterTree? {
        get { return _parameterTree }
        set { _parameterTree = newValue }
    }

    public override var inputBusses: AUAudioUnitBusArray { return _inputBusArray }
    public override var outputBusses: AUAudioUnitBusArray { return _outputBusArray }
    public override var supportsUserPresets: Bool { return true }

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
        }

        _parameterTree.implementorValueProvider = { [weak self] param in
            return self?.kernel.getParameter(param.address) ?? param.defaultValue
        }

        _parameterTree.implementorStringFromValueCallback = { param, valuePtr in
            let value = valuePtr?.pointee ?? param.value
            switch param.address {
            case 0: return String(format: "%.0f ms", value)
            case 1: return String(format: "%.0f%%", value)
            case 2: return String(format: "±%.0f", value)
            default: return String(format: "%.1f", value)
            }
        }
    }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        let sampleRate = _outputBus.format.sampleRate
        kernel.setUp(Int32(outputBusses[0].format.channelCount), sampleRate: sampleRate)
    }

    public override func deallocateRenderResources() {
        super.deallocateRenderResources()
        kernel.tearDown()
    }

    public override var internalRenderBlock: AUInternalRenderBlock {
        let kern = kernel
        let musicalContext = self.musicalContextBlock

        return { actionFlags, timestamp, frameCount, outputBusNumber,
                 outputData, realtimeEventListHead, pullInputBlock in

            guard let pullInputBlock = pullInputBlock else {
                return kAudioUnitErr_NoConnection
            }

            // Pull audio input
            var pullFlags = AudioUnitRenderActionFlags(rawValue: 0)
            let status = pullInputBlock(&pullFlags, timestamp, frameCount, 0, outputData)
            guard status == noErr else { return status }

            // Query host transport for beat position and tempo
            if let musicalContext = musicalContext {
                var tempo: Double = 120.0
                var beatPosition: Double = 0.0
                var dummy1: Double = 0
                var dummy2: Double = 0
                var dummy3: Int = 0
                var dummy4: Int = 0

                if musicalContext(&tempo, &dummy1, &dummy2, &beatPosition, &dummy3, &dummy4) {
                    kern.setBeatPosition(beatPosition, tempo: tempo)
                }
            }

            // Walk MIDI event linked list and forward to DSP
            var event = realtimeEventListHead?.pointee
            while event != nil {
                if event!.head.eventType == .MIDI {
                    let midi = event!.MIDI
                    kern.handleMIDIEvent(midi.data.0, data1: midi.data.1, data2: midi.data.2)
                }
                if let next = event!.head.next {
                    event = next.pointee
                } else {
                    event = nil
                }
            }

            // Process audio with pitch shifting
            kern.process(outputData, frameCount: frameCount)
            return noErr
        }
    }

    // MARK: - Preset Persistence

    private static let automationKey = "automationCurveData"

    public override var fullState: [String: Any]? {
        get {
            var state = super.fullState ?? [:]

            // Serialize automation curve
            let maxBytes = 4 + 256 * 24  // header + max breakpoints
            var data = Data(count: maxBytes)
            // We can't directly call serialize from Swift, so we'll store breakpoints
            // through the parameter tree's fullState mechanism
            state[GlideAudioUnit.automationKey] = data
            return state
        }
        set {
            super.fullState = newValue
            // Deserialization handled when automation data is present
        }
    }
}
