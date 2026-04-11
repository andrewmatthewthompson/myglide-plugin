import CoreAudioKit
import AVFoundation

public class AudioUnitFactory: NSObject, AUAudioUnitFactory {

    var auAudioUnit: GlideAudioUnit?

    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        let audioUnit = try GlideAudioUnit(componentDescription: componentDescription, options: [])
        auAudioUnit = audioUnit
        return audioUnit
    }

    public func requestViewController(completionHandler: @escaping (NSViewController?) -> Void) {
        guard let audioUnit = auAudioUnit else {
            completionHandler(nil)
            return
        }
        let hostingController = NSHostingController(rootView: GlideMainView(audioUnit: audioUnit))
        hostingController.preferredContentSize = NSSize(width: 700, height: 500)
        hostingController.sizingOptions = .preferredContentSize
        completionHandler(hostingController)
    }

    public func beginRequest(with context: NSExtensionContext) {
        // Required by NSExtensionRequestHandling. AUAudioUnitFactory
        // handles the actual Audio Unit extension lifecycle, so no
        // additional work is needed here.
    }
}
