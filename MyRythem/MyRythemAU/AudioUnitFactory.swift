import CoreAudioKit
import AVFoundation
import SwiftUI

/// AUv3 principal class. For a `com.apple.AudioUnit-UI` extension the
/// principal class must be an `AUViewController` that also conforms to
/// `AUAudioUnitFactory`; the same instance vends the audio unit and
/// provides the plugin UI.
public class AudioUnitFactory: AUViewController, AUAudioUnitFactory {

    private var auAudioUnit: RythemAudioUnit?
    private var didInstallSwiftUIView = false

    public override func loadView() {
        view = NSView(frame: NSRect(x: 0, y: 0, width: 640, height: 420))
        preferredContentSize = NSSize(width: 640, height: 420)
    }

    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        let audioUnit = try RythemAudioUnit(componentDescription: componentDescription, options: [])
        auAudioUnit = audioUnit

        DispatchQueue.main.async { [weak self] in
            guard let self = self, self.isViewLoaded else { return }
            self.installSwiftUIViewIfNeeded(audioUnit: audioUnit)
        }

        return audioUnit
    }

    public override func viewDidLoad() {
        super.viewDidLoad()
        if let audioUnit = auAudioUnit {
            installSwiftUIViewIfNeeded(audioUnit: audioUnit)
        }
    }

    private func installSwiftUIViewIfNeeded(audioUnit: RythemAudioUnit) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !didInstallSwiftUIView else { return }
        didInstallSwiftUIView = true

        let host = NSHostingController(rootView: RythemMainView(audioUnit: audioUnit))
        addChild(host)
        host.view.frame = view.bounds
        host.view.autoresizingMask = [.width, .height]
        view.addSubview(host.view)
    }
}
