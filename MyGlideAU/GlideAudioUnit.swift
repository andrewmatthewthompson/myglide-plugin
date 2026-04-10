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

    // MARK: - Latency & Tail Time (for Logic Pro PDC and offline bounce)

    /// Latency in seconds introduced by the granular pitch shifter (one grain duration).
    /// Logic Pro uses this to align this track with others via Plugin Delay Compensation.
    public override var latency: TimeInterval {
        return TimeInterval(kernel.latencySamples()) / (_outputBus?.format.sampleRate ?? 48000.0)
    }

    /// Tail time in seconds: how long output continues after input stops.
    /// Logic Pro uses this during offline bounce to capture the full tail.
    public override var tailTime: TimeInterval {
        return kernel.tailTimeSeconds()
    }

    // MARK: - Bypass

    /// When true, the render block passes audio through unprocessed with zero latency.
    /// Logic Pro sets this when the user clicks the bypass button on the plugin slot.
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
        // Capture an Unmanaged reference to avoid ARC retain/release on the audio thread.
        let kernRef = Unmanaged.passUnretained(kernel)
        let musicalContext = self.musicalContextBlock

        // Capture bypass flag pointer for lock-free audio-thread read.
        // shouldBypassEffect is set from the main thread by Logic Pro;
        // we read it each block without synchronization (benign race on Bool).
        let bypassRef = UnsafeMutablePointer<Bool>(&_shouldBypassEffect)

        return { actionFlags, timestamp, frameCount, outputBusNumber,
                 outputData, realtimeEventListHead, pullInputBlock in

            guard let pullInputBlock = pullInputBlock else {
                return kAudioUnitErr_NoConnection
            }

            // Pull audio input into outputData (in-place processing)
            var pullFlags = AudioUnitRenderActionFlags(rawValue: 0)
            let status = pullInputBlock(&pullFlags, timestamp, frameCount, 0, outputData)
            guard status == noErr else { return status }

            // Bypass: pass audio through unprocessed (zero latency, zero CPU)
            if bypassRef.pointee {
                return noErr
            }

            let kern = kernRef.takeUnretainedValue()

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
            var eventPtr = realtimeEventListHead
            while let event = eventPtr {
                if event.pointee.head.eventType == .MIDI {
                    let midi = event.pointee.MIDI
                    kern.handleMIDIEvent(midi.data.0, data1: midi.data.1, data2: midi.data.2)
                }
                eventPtr = event.pointee.head.next
            }

            // Process audio with pitch shifting
            kern.process(outputData, frameCount: frameCount)
            return noErr
        }
    }
}
