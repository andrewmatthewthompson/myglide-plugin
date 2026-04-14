import AVFoundation
import AudioToolbox

/// AUv3 MIDI processor (`aumi`). Receives MIDI, tracks held chord, and
/// emits re-gated chord hits on a rhythmic step pattern.
public class RythemAudioUnit: AUAudioUnit {

    private var _parameterTree: AUParameterTree!
    private var _inputBus: AUAudioUnitBus!
    private var _outputBus: AUAudioUnitBus!
    private var _inputBusArray: AUAudioUnitBusArray!
    private var _outputBusArray: AUAudioUnitBusArray!

    let kernel = RythemDSPKernelBridge()

    /// Fired after fullState restore so the UI can refresh.
    var onStateRestored: (() -> Void)?

    public override var parameterTree: AUParameterTree? {
        get { return _parameterTree }
        set { _parameterTree = newValue }
    }

    public override var inputBusses: AUAudioUnitBusArray  { return _inputBusArray }
    public override var outputBusses: AUAudioUnitBusArray { return _outputBusArray }
    public override var supportsUserPresets: Bool         { return true }

    // MARK: - MIDI I/O declaration

    public override var midiOutputNames: [String] { return ["MIDI Out"] }

    // MARK: - Bypass

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
        _inputBus  = try AUAudioUnitBus(format: format)
        _outputBus = try AUAudioUnitBus(format: format)
        _inputBusArray  = AUAudioUnitBusArray(audioUnit: self, busType: .input,  busses: [_inputBus])
        _outputBusArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [_outputBus])

        setupParameterTree()
    }

    private func setupParameterTree() {
        let params = RythemParameters.createParameters()
        _parameterTree = AUParameterTree.createTree(withChildren: params)

        _parameterTree.implementorValueObserver = { [weak self] param, value in
            self?.kernel.setParameter(param.address, value: value)
        }

        func lookup(_ names: [String], _ value: Float) -> String {
            let idx = Int(value.rounded())
            if idx >= 0 && idx < names.count { return names[idx] }
            return "?"
        }

        _parameterTree.implementorStringFromValueCallback = { param, valuePtr in
            let value = valuePtr?.pointee ?? param.value
            switch param.address {
            case RythemParameters.Address.pattern.rawValue:
                return lookup(RythemParameters.patternNames, value)
            case RythemParameters.Address.rate.rawValue:
                return lookup(RythemParameters.rateNames, value)
            case RythemParameters.Address.gate.rawValue:
                return String(format: "%.0f%%", value)
            case RythemParameters.Address.swing.rawValue:
                return String(format: "%.0f%%", value)
            case RythemParameters.Address.velocityMode.rawValue:
                return lookup(RythemParameters.velocityModeNames, value)
            case RythemParameters.Address.fixedVelocity.rawValue:
                return String(format: "%.0f", value)
            case RythemParameters.Address.accent.rawValue:
                return String(format: "%.0f%%", value)
            case RythemParameters.Address.octave.rawValue:
                return String(format: "%+.0f oct", value)
            case RythemParameters.Address.latch.rawValue:
                return value > 0.5 ? "On" : "Off"
            case RythemParameters.Address.syncMode.rawValue:
                return lookup(RythemParameters.syncModeNames, value)
            case RythemParameters.Address.freeBPM.rawValue:
                return String(format: "%.0f BPM", value)
            case RythemParameters.Address.releaseMode.rawValue:
                return lookup(RythemParameters.releaseModeNames, value)
            case RythemParameters.Address.patternLength.rawValue:
                return String(format: "%.0f", value)
            default:
                return String(format: "%.2f", value)
            }
        }
    }

    // MARK: - Render Resources

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        let sampleRate = _outputBus.format.sampleRate
        kernel.setUp(Int32(outputBusses[0].format.channelCount), sampleRate: sampleRate)
        pushParameterValuesToKernel()
    }

    private func pushParameterValuesToKernel() {
        guard let tree = _parameterTree else { return }
        for param in tree.allParameters {
            kernel.setParameter(param.address, value: param.value)
        }
    }

    public override func deallocateRenderResources() {
        super.deallocateRenderResources()
        kernel.tearDown()
    }

    // MARK: - Render Block

    public override var internalRenderBlock: AUInternalRenderBlock {
        let kernRef        = Unmanaged.passUnretained(kernel)
        let musicalContext = self.musicalContextBlock
        let bypassRef      = _bypassFlag

        return { [weak self] actionFlags, timestamp, frameCount, outputBusNumber,
                 outputData, realtimeEventListHead, pullInputBlock in

            // Pass audio through (silent/unchanged). This is a MIDI plugin,
            // but the render contract still requires output buffers.
            if let pullInputBlock = pullInputBlock {
                var pullFlags = AudioUnitRenderActionFlags(rawValue: 0)
                let status = pullInputBlock(&pullFlags, timestamp, frameCount, 0, outputData)
                if status != noErr { return status }
            } else {
                // No input connection — fill with silence.
                let abl = UnsafeMutableAudioBufferListPointer(outputData)
                for buf in abl {
                    if let data = buf.mData {
                        memset(data, 0, Int(buf.mDataByteSize))
                    }
                }
            }

            let kern = kernRef.takeUnretainedValue()

            if bypassRef.pointee {
                kern.killAllSounding(0)
                return noErr
            }

            // Host transport
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

            // Feed input MIDI events to the kernel. Held-note tracking
            // doesn't need sub-block timestamping — all notes arriving in
            // a block contribute to the chord that fires on the next step.
            var eventPtr: UnsafePointer<AURenderEvent>? = realtimeEventListHead
            while let event = eventPtr {
                if event.pointee.head.eventType == .MIDI {
                    let midi = event.pointee.MIDI
                    kern.handleMIDIEvent(midi.data.0, data1: midi.data.1, data2: midi.data.2,
                                         frameOffset: 0)
                }
                eventPtr = event.pointee.head.next.map { UnsafePointer($0) }
            }

            // Advance step clock + build output MIDI
            kern.process(frameCount)

            // Dispatch output MIDI through the host's MIDI out block.
            if let emit = self?.midiOutputEventBlock {
                let count = kern.pendingMIDIEventCount()
                if count > 0 {
                    var status: UInt8 = 0
                    var d1: UInt8 = 0
                    var d2: UInt8 = 0
                    var off: UInt32 = 0
                    for i in 0..<count {
                        kern.pendingMIDIEventAtIndex(Int32(i),
                                                     status: &status,
                                                     data1: &d1,
                                                     data2: &d2,
                                                     frameOffset: &off)
                        let eventTime = timestamp.pointee.mSampleTime + Double(off)
                        let bytes: [UInt8] = [status, d1, d2]
                        _ = bytes.withUnsafeBufferPointer { buf in
                            emit(AUEventSampleTime(eventTime), 0, 3, buf.baseAddress!)
                        }
                    }
                }
            }

            return noErr
        }
    }
}
