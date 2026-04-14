import SwiftUI
import AVFoundation

/// Minimal placeholder UI. Just enough to prove the plugin loads and the
/// parameter tree is live. Will be expanded into a full step-grid editor.
struct RythemMainView: View {

    let audioUnit: RythemAudioUnit

    @State private var patternIndex: Float = 3
    @State private var rateIndex: Float    = 3
    @State private var gatePct: Float      = 60
    @State private var swingPct: Float     = 50
    @State private var octave: Float       = 0
    @State private var latchOn: Bool       = false

    @State private var heldCount: Int      = 0
    @State private var currentStep: Int    = 0
    @State private var stepIsActive: Bool  = false

    private let refreshTimer = Timer.publish(every: 1.0 / 30.0, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            header
            Divider()
            controls
            Divider()
            stepStrip
            Spacer()
            statusBar
        }
        .padding(16)
        .frame(minWidth: 500, minHeight: 320)
        .onAppear { syncFromParams() }
        .onReceive(refreshTimer) { _ in
            heldCount    = Int(audioUnit.kernel.heldNoteCount())
            currentStep  = Int(audioUnit.kernel.currentStepIndex())
            stepIsActive = audioUnit.kernel.stepIsActiveNow()
        }
    }

    // MARK: - Subviews

    private var header: some View {
        HStack(spacing: 12) {
            Image(systemName: "metronome.fill").font(.title2)
            Text("MyRythem")
                .font(.title2)
                .fontWeight(.semibold)
            Spacer()
            Text("Chord → Rhythm")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 20) {
                picker("Pattern", selection: $patternIndex,
                       options: RythemParameters.patternNames,
                       address: .pattern)
                picker("Rate", selection: $rateIndex,
                       options: RythemParameters.rateNames,
                       address: .rate)
            }

            slider("Gate", value: $gatePct, range: 5...100,
                   unit: "%", address: .gate)
            slider("Swing", value: $swingPct, range: 50...75,
                   unit: "%", address: .swing)

            HStack(spacing: 20) {
                Stepper(value: $octave, in: -2...2, step: 1) {
                    Text("Octave: \(Int(octave))")
                        .monospacedDigit()
                }
                .onChange(of: octave) { newValue in
                    setParam(.octave, Float(newValue))
                }

                Toggle("Latch", isOn: $latchOn)
                    .onChange(of: latchOn) { newValue in
                        setParam(.latch, newValue ? 1.0 : 0.0)
                    }
            }
        }
    }

    private var stepStrip: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Step").font(.caption).foregroundColor(.secondary)
            HStack(spacing: 4) {
                ForEach(0..<16, id: \.self) { i in
                    RoundedRectangle(cornerRadius: 3)
                        .fill(cellColor(i))
                        .frame(height: 24)
                }
            }
        }
    }

    private var statusBar: some View {
        HStack {
            Label("\(heldCount) held", systemImage: "pianokeys")
            Spacer()
            Text("Step \(currentStep + 1)/16")
                .monospacedDigit()
                .foregroundColor(stepIsActive ? .accentColor : .secondary)
        }
        .font(.caption)
    }

    // MARK: - Helpers

    private func cellColor(_ i: Int) -> Color {
        if i == currentStep { return .accentColor }
        return Color.gray.opacity(0.2)
    }

    private func picker(_ title: String, selection: Binding<Float>,
                        options: [String],
                        address: RythemParameters.Address) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundColor(.secondary)
            Picker("", selection: selection) {
                ForEach(0..<options.count, id: \.self) { idx in
                    Text(options[idx]).tag(Float(idx))
                }
            }
            .labelsHidden()
            .onChange(of: selection.wrappedValue) { newValue in
                setParam(address, newValue)
            }
        }
    }

    private func slider(_ title: String, value: Binding<Float>,
                        range: ClosedRange<Float>, unit: String,
                        address: RythemParameters.Address) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(title).font(.caption).foregroundColor(.secondary)
                Spacer()
                Text("\(Int(value.wrappedValue))\(unit)")
                    .font(.caption.monospacedDigit())
                    .foregroundColor(.secondary)
            }
            Slider(value: value, in: range) { _ in
                setParam(address, value.wrappedValue)
            }
        }
    }

    private func setParam(_ address: RythemParameters.Address, _ value: Float) {
        audioUnit.parameterTree?.parameter(withAddress: address.rawValue)?.value = value
    }

    private func syncFromParams() {
        guard let tree = audioUnit.parameterTree else { return }
        patternIndex = tree.parameter(withAddress: RythemParameters.Address.pattern.rawValue)?.value ?? 3
        rateIndex    = tree.parameter(withAddress: RythemParameters.Address.rate.rawValue)?.value ?? 3
        gatePct      = tree.parameter(withAddress: RythemParameters.Address.gate.rawValue)?.value ?? 60
        swingPct     = tree.parameter(withAddress: RythemParameters.Address.swing.rawValue)?.value ?? 50
        octave       = tree.parameter(withAddress: RythemParameters.Address.octave.rawValue)?.value ?? 0
        let latch    = tree.parameter(withAddress: RythemParameters.Address.latch.rawValue)?.value ?? 0
        latchOn      = latch > 0.5
    }
}
