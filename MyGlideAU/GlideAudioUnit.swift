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

    private var _shouldBypassEffect = false
    public override var shouldBypassEffect: Bool {
        get { return _shouldBypassEffect }
        set { _shouldBypassEffect = newValue }
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
        }

        _parameterTree.implementorValueProvider = { [weak self] param in
            return self?.kernel.getParameter(param.address) ?? param.defaultValue
        }

        _parameterTree.implementorStringFromValueCallback = { param, valuePtr in
            let value = valuePtr?.pointee ?? param.value
            switch param.address {
            case 0: return String(format: "%.0f ms", value)
            case 1: return String(format: "%.0f%%", value)
            case 2: return String(format: "\u{00B1}%.0f", value)
            default: return String(format: "%.1f", value)
            }
        }
    }

    // MARK: - Preset Persistence (fullState)

    private static let automationKey = "automationCurveData"

    public override var fullState: [String: Any]? {
        get {
            var state = super.fullState ?? [:]
            let data = kernel.automationSerialize()
            if data.count > 0 {
                state[GlideAudioUnit.automationKey] = data
            }
            return state
        }
        set {
            super.fullState = newValue
            if let data = newValue?[GlideAudioUnit.automationKey] as? Data {
                kernel.automationDeserialize(fromData: data)
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
        let bypassRef = UnsafeMutablePointer<Bool>(&_shouldBypassEffect)

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
                var dummy1: Double = 0
                var dummy2: Double = 0
                var dummy3: Int = 0
                var dummy4: Int = 0

                if musicalContext(&tempo, &dummy1, &dummy2, &beatPosition, &dummy3, &dummy4) {
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
