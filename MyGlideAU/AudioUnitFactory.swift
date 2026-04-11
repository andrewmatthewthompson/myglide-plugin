import CoreAudioKit
import AVFoundation
import SwiftUI

/// AUv3 principal class. For a `com.apple.AudioUnit-UI` extension the
/// principal class must be an `AUViewController` that also conforms to
/// `AUAudioUnitFactory`; the same instance vends the audio unit and
/// provides the plugin UI.
public class AudioUnitFactory: AUViewController, AUAudioUnitFactory {

    private var auAudioUnit: GlideAudioUnit?

    public override func loadView() {
        // Supply a plain container view; the SwiftUI UI is installed
        // once `createAudioUnit(with:)` has produced the audio unit.
        view = NSView(frame: NSRect(x: 0, y: 0, width: 700, height: 500))
        preferredContentSize = NSSize(width: 700, height: 500)
    }

    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        let audioUnit = try GlideAudioUnit(componentDescription: componentDescription, options: [])
        auAudioUnit = audioUnit
        if isViewLoaded {
            installSwiftUIView(audioUnit: audioUnit)
        }
        return audioUnit
    }

    public override func viewDidLoad() {
        super.viewDidLoad()
        if let audioUnit = auAudioUnit {
            installSwiftUIView(audioUnit: audioUnit)
        }
    }

    private func installSwiftUIView(audioUnit: GlideAudioUnit) {
        let host = NSHostingController(rootView: GlideMainView(audioUnit: audioUnit))
        addChild(host)
        host.view.frame = view.bounds
        host.view.autoresizingMask = [.width, .height]
        view.addSubview(host.view)
    }
}
