import CoreAudioKit
import AVFoundation
import SwiftUI

/// AUv3 principal class. For a `com.apple.AudioUnit-UI` extension the
/// principal class must be an `AUViewController` that also conforms to
/// `AUAudioUnitFactory`; the same instance vends the audio unit and
/// provides the plugin UI.
public class AudioUnitFactory: AUViewController, AUAudioUnitFactory {

    private var auAudioUnit: GlideAudioUnit?
    private var didInstallSwiftUIView = false

    public override func loadView() {
        // Supply a plain container view; the SwiftUI UI is installed
        // once `createAudioUnit(with:)` has produced the audio unit.
        view = NSView(frame: NSRect(x: 0, y: 0, width: 700, height: 500))
        preferredContentSize = NSSize(width: 700, height: 500)
    }

    /// Called by the host (on an XPC background thread) to instantiate
    /// the audio unit. We must NOT touch the view hierarchy here — any
    /// AppKit mutation has to happen on the main thread.
    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        let audioUnit = try GlideAudioUnit(componentDescription: componentDescription, options: [])
        auAudioUnit = audioUnit

        // If the view has already been loaded on the main thread, hop
        // back to it to install the SwiftUI content. Otherwise, wait
        // for viewDidLoad to do it.
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

    /// Installs the SwiftUI view. Must be called on the main thread.
    /// Safe to call multiple times — subsequent calls are no-ops.
    private func installSwiftUIViewIfNeeded(audioUnit: GlideAudioUnit) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !didInstallSwiftUIView else { return }
        didInstallSwiftUIView = true

        let host = NSHostingController(rootView: GlideMainView(audioUnit: audioUnit))
        addChild(host)
        host.view.frame = view.bounds
        host.view.autoresizingMask = [.width, .height]
        view.addSubview(host.view)
    }
}
