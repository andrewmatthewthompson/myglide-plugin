import SwiftUI
import AVFoundation

/// Observable bridge between SwiftUI knobs and AUParameterTree.
/// Two-way binding pattern proven in MyVerb.
class GlideParameterState: ObservableObject {
    @Published var glideTime: Double = 200.0
    @Published var mix: Double = 100.0

    private var parameterTree: AUParameterTree?
    private var observerToken: AUParameterObserverToken?
    private var isExternalUpdate = false

    func attach(to audioUnit: GlideAudioUnit) {
        guard let tree = audioUnit.parameterTree else { return }
        parameterTree = tree

        // Host automation → UI
        observerToken = tree.token(byAddingParameterObserver: { [weak self] address, value in
            DispatchQueue.main.async {
                self?.update(address: address, value: Double(value))
            }
        })

        // Read initial values
        for param in tree.allParameters {
            update(address: param.address, value: Double(param.value))
        }
    }

    private func update(address: AUParameterAddress, value: Double) {
        isExternalUpdate = true
        switch address {
        case 0: glideTime = value
        case 1: mix = value
        default: break
        }
        isExternalUpdate = false
    }

    func sendIfLocal(_ address: AUParameterAddress, value: Double) {
        guard !isExternalUpdate else { return }
        parameterTree?.parameter(withAddress: address)?.value = AUValue(value)
    }
}

struct GlideMainView: View {
    @StateObject private var state = GlideParameterState()
    let audioUnit: GlideAudioUnit

    var body: some View {
        ZStack {
            // Background gradient — teal/blue theme for glide
            LinearGradient(
                colors: [
                    Color(red: 0.08, green: 0.12, blue: 0.22),
                    Color(red: 0.10, green: 0.20, blue: 0.35),
                    Color(red: 0.12, green: 0.28, blue: 0.42)
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )

            VStack(spacing: 20) {
                // Header
                HStack {
                    Text("MYGLIDE")
                        .font(.system(size: 18, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                    Spacer()
                    Text("Portamento")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.6))
                }
                .padding(.horizontal, 24)
                .padding(.top, 16)

                // Knobs
                HStack(spacing: 40) {
                    KnobView(
                        label: "GLIDE",
                        value: $state.glideTime,
                        range: 1...2000,
                        unit: "ms",
                        address: 0,
                        state: state
                    )
                    KnobView(
                        label: "MIX",
                        value: $state.mix,
                        range: 0...100,
                        unit: "%",
                        address: 1,
                        state: state
                    )
                }
                .padding(.bottom, 16)
            }
        }
        .frame(width: 460, height: 240)
        .onAppear { state.attach(to: audioUnit) }
    }
}

struct KnobView: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let unit: String
    let address: AUParameterAddress
    let state: GlideParameterState

    @State private var isDragging = false

    private var normalized: Double {
        (value - range.lowerBound) / (range.upperBound - range.lowerBound)
    }

    private var angle: Double {
        -135.0 + normalized * 270.0
    }

    var body: some View {
        VStack(spacing: 8) {
            ZStack {
                // Track arc
                Circle()
                    .trim(from: 0.125, to: 0.875)
                    .rotation(.degrees(90))
                    .stroke(Color.white.opacity(0.15), lineWidth: 4)
                    .frame(width: 64, height: 64)

                // Value arc
                Circle()
                    .trim(from: 0.125, to: 0.125 + normalized * 0.75)
                    .rotation(.degrees(90))
                    .stroke(
                        Color(red: 0.3, green: 0.7, blue: 0.9),
                        style: StrokeStyle(lineWidth: 4, lineCap: .round)
                    )
                    .frame(width: 64, height: 64)

                // Pointer
                Circle()
                    .fill(isDragging ? Color.white : Color.white.opacity(0.8))
                    .frame(width: 6, height: 6)
                    .offset(y: -28)
                    .rotationEffect(.degrees(angle))
            }
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { gesture in
                        isDragging = true
                        let delta = -gesture.translation.height / 150.0
                        let span = range.upperBound - range.lowerBound
                        let newValue = (value + delta * span).clamped(to: range)
                        value = newValue
                        state.sendIfLocal(address, value: value)
                    }
                    .onEnded { _ in isDragging = false }
            )

            // Value display
            Text(formatValue())
                .font(.system(size: 12, weight: .medium, design: .monospaced))
                .foregroundColor(.white)

            // Label
            Text(label)
                .font(.system(size: 10, weight: .semibold))
                .foregroundColor(.white.opacity(0.6))
        }
    }

    private func formatValue() -> String {
        if range.upperBound >= 1000 {
            return String(format: "%.0f %@", value, unit)
        }
        return String(format: "%.1f %@", value, unit)
    }
}

private extension Double {
    func clamped(to range: ClosedRange<Double>) -> Double {
        return Swift.min(Swift.max(self, range.lowerBound), range.upperBound)
    }
}
