import AVFoundation
import AudioToolbox

public class GlideAudioUnit: AUAudioUnit {

    private var _parameterTree: AUParameterTree!
    private var _inputBus: AUAudioUnitBus!
    private var _outputBus: AUAudioUnitBus!
    private var _inputBusArray: AUAudioUnitBusArray!
    private var _outputBusArray: AUAudioUnitBusArray!

    private let kernel = GlideDSPKernelBridge()

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
            return String(format: "%.1f", value)
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

        return { [weak self] actionFlags, timestamp, frameCount, outputBusNumber,
                  outputData, realtimeEventListHead, pullInputBlock in

            guard let pullInputBlock = pullInputBlock else {
                return kAudioUnitErr_NoConnection
            }

            var pullFlags = AudioUnitRenderActionFlags(rawValue: 0)
            let status = pullInputBlock(&pullFlags, timestamp, frameCount, 0, outputData)
            guard status == noErr else { return status }

            kern.process(outputData, frameCount: frameCount)
            return noErr
        }
    }
}
