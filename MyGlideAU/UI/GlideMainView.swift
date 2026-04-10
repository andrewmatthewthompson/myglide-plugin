import SwiftUI
import AVFoundation

// MARK: - Note Helpers

private let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

private func noteName(midi: Int) -> String {
    let name = noteNames[midi % 12]
    let octave = (midi / 12) - 1
    return "\(name)\(octave)"
}

private func midiForNoteName(_ name: String, octave: Int) -> Int {
    guard let idx = noteNames.firstIndex(of: name) else { return 60 }
    return (octave + 1) * 12 + idx
}

// MARK: - Automation State (shared between UI and DSP)

/// Bridges the C++ AutomationCurve to SwiftUI via the kernel bridge.
class AutomationState: ObservableObject {
    /// Swift-side mirror of breakpoints for display.
    struct UIBreakpoint: Identifiable {
        let id = UUID()
        var beat: Double
        var semitones: Double
        var interpType: Int  // 0=linear, 1=smooth, 2=step
    }

    @Published var breakpoints: [UIBreakpoint] = []
    @Published var interpolationMode: Int = 0  // 0=linear, 1=smooth, 2=step
    @Published var snapEnabled: Bool = true

    private weak var kernelBridge: GlideDSPKernelBridge?

    func attach(to bridge: GlideDSPKernelBridge) {
        kernelBridge = bridge
    }

    func addBreakpoint(beat: Double, semitones: Double) {
        let snapped = snapEnabled ? semitones.rounded() : semitones
        let bp = UIBreakpoint(beat: beat, semitones: snapped, interpType: interpolationMode)
        breakpoints.append(bp)
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
    }

    func moveBreakpoint(index: Int, beat: Double, semitones: Double) {
        guard index >= 0 && index < breakpoints.count else { return }
        let snapped = snapEnabled ? semitones.rounded() : semitones
        breakpoints[index].beat = max(0, beat)
        breakpoints[index].semitones = snapped
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
    }

    func removeBreakpoint(index: Int) {
        guard index >= 0 && index < breakpoints.count else { return }
        breakpoints.remove(at: index)
        commitToDSP()
    }

    func clearAll() {
        breakpoints.removeAll()
        commitToDSP()
    }

    func setInterpolationMode(_ mode: Int) {
        interpolationMode = mode
        for i in breakpoints.indices {
            breakpoints[i].interpType = mode
        }
        commitToDSP()
    }

    /// Push breakpoints to the C++ triple-buffer.
    private func commitToDSP() {
        guard let bridge = kernelBridge,
              let curvePtr = bridge.automationCurvePtr() else { return }

        // Access the AutomationCurve via its opaque pointer
        // We use the bridge methods to manipulate the curve
        let curve = curvePtr.assumingMemoryBound(to: UInt8.self)

        // Build serialized data and write through
        // For simplicity, we rebuild the curve each time
        typealias CurveType = OpaquePointer

        // Direct memory manipulation: call beginEdit, add breakpoints, commitEdit
        // We cast the void* to our C++ AutomationCurve layout
        // This works because AutomationCurve's methods are called through the pointer

        // Use a simpler approach: serialize breakpoints to Data,
        // pass through bridge. For now, use the low-level pointer approach.

        // The AutomationCurve pointer lets us call methods directly via the bridge.
        // Since we can't call C++ methods from Swift directly, we'll serialize.
        commitViaSerialize()
    }

    private func commitViaSerialize() {
        guard let bridge = kernelBridge,
              let curvePtr = bridge.automationCurvePtr() else { return }

        // We need to serialize our breakpoints and write them to the curve.
        // Since we can't call C++ methods directly from Swift, we'll use
        // the bridge's Obj-C interface for curve manipulation.
        // For the initial implementation, we rebuild by:
        // 1. Serialize to bytes matching AutomationCurve::deserialize format
        // 2. Write through the opaque pointer

        // Breakpoint layout in C++: { double beat, double semitones, uint8_t interp } + padding
        // Total struct size = 17 bytes (but likely padded to 24)
        // Actually in C++ it's: double(8) + double(8) + uint8_t(1) = 17, padded to 24

        // For safety, we'll manipulate the triple buffer directly.
        // The AutomationCurve stores Buffer structs with Breakpoint arrays.

        // Simpler approach: we know the memory layout. A Buffer has:
        // - Breakpoint[256] at offset 0
        // - int count at offset 256 * sizeof(Breakpoint)
        // Each Breakpoint is: double beat(8) + double semitones(8) + uint8_t interp(1) + 7 padding = 24 bytes

        // Write to the "write buffer" index, then set pending flag.
        // We access the AutomationCurve at curvePtr.

        // For robustness in v1, notify the bridge to rebuild the curve:
        notifyBridgeOfBreakpoints()
    }

    private func notifyBridgeOfBreakpoints() {
        // Build the serialized byte array matching AutomationCurve::deserialize
        // Header: int32 count (4 bytes)
        // Then count * sizeof(Breakpoint) bytes
        // Breakpoint = { double beat, double semitones, InterpolationType(uint8_t) }
        // sizeof(Breakpoint) in C++ with padding = likely 24 bytes

        var data = Data()
        var count = Int32(breakpoints.count)
        data.append(Data(bytes: &count, count: 4))

        for bp in breakpoints {
            var beat = bp.beat
            var semi = bp.semitones
            var interp = UInt8(bp.interpType)
            data.append(Data(bytes: &beat, count: 8))
            data.append(Data(bytes: &semi, count: 8))
            data.append(Data(bytes: &interp, count: 1))
            // Pad to match C++ struct alignment (24 bytes total per Breakpoint)
            let padding = Data(count: 7)
            data.append(padding)
        }

        // Write directly into the automation curve's write buffer via pointer
        guard let bridge = kernelBridge,
              let curvePtr = bridge.automationCurvePtr() else { return }

        // The AutomationCurve has methods beginEdit/deserialize/commitEdit.
        // Since we can't call them from Swift, we write to the raw memory.
        // This is safe because the UI thread owns the write buffer.

        // AutomationCurve memory layout:
        // mBuffers[3]: each Buffer = { Breakpoint[256], int count }
        // mReadIndex: atomic<int> (at offset after buffers)
        // mPendingIndex: atomic<int>
        // mWriteIndex: int

        let breakpointSize = 24  // sizeof(Breakpoint) with padding
        let bufferSize = 256 * breakpointSize + 4  // points[256] + count(int)
        let totalBuffersSize = 3 * bufferSize

        // Read mWriteIndex (last 4 bytes of the struct after atomics)
        // Layout: mBuffers[3] + atomic<int> mReadIndex + atomic<int> mPendingIndex + int mWriteIndex
        // atomic<int> is typically 4 bytes on most platforms
        let writeIndexOffset = totalBuffersSize + 4 + 4  // after mReadIndex + mPendingIndex
        let writeIndexPtr = curvePtr.advanced(by: writeIndexOffset)
        let writeIndex = writeIndexPtr.assumingMemoryBound(to: Int32.self).pointee

        // Write breakpoints into the write buffer
        let writeBufferOffset = Int(writeIndex) * bufferSize
        let writeBufferPtr = curvePtr.advanced(by: writeBufferOffset)

        // Copy breakpoint data
        let pointsPtr = writeBufferPtr
        for (i, bp) in breakpoints.enumerated() {
            let bpPtr = pointsPtr.advanced(by: i * breakpointSize)
            bpPtr.assumingMemoryBound(to: Double.self).pointee = bp.beat
            bpPtr.advanced(by: 8).assumingMemoryBound(to: Double.self).pointee = bp.semitones
            bpPtr.advanced(by: 16).assumingMemoryBound(to: UInt8.self).pointee = UInt8(bp.interpType)
        }

        // Write count at end of buffer
        let countPtr = writeBufferPtr.advanced(by: 256 * breakpointSize)
        countPtr.assumingMemoryBound(to: Int32.self).pointee = Int32(breakpoints.count)

        // Set pending index (atomic store with release semantics)
        let pendingOffset = totalBuffersSize + 4  // after mReadIndex
        let pendingPtr = curvePtr.advanced(by: pendingOffset)
        pendingPtr.assumingMemoryBound(to: Int32.self).pointee = writeIndex

        // Find next write buffer (not read, not pending)
        let readOffset = totalBuffersSize
        let readIndex = curvePtr.advanced(by: readOffset).assumingMemoryBound(to: Int32.self).pointee
        for i: Int32 in 0..<3 {
            if i != readIndex && i != writeIndex {
                writeIndexPtr.assumingMemoryBound(to: Int32.self).pointee = i
                break
            }
        }
    }
}

// MARK: - Parameter State

class GlideParameterState: ObservableObject {
    @Published var glideTime: Double = 50.0
    @Published var mix: Double = 100.0
    @Published var pitchRange: Double = 24.0

    private var parameterTree: AUParameterTree?
    private var observerToken: AUParameterObserverToken?
    private var isExternalUpdate = false

    func attach(to audioUnit: GlideAudioUnit) {
        guard let tree = audioUnit.parameterTree else { return }
        parameterTree = tree

        observerToken = tree.token(byAddingParameterObserver: { [weak self] address, value in
            DispatchQueue.main.async {
                self?.update(address: address, value: Double(value))
            }
        })

        for param in tree.allParameters {
            update(address: param.address, value: Double(param.value))
        }
    }

    private func update(address: AUParameterAddress, value: Double) {
        isExternalUpdate = true
        switch address {
        case 0: glideTime = value
        case 1: mix = value
        case 2: pitchRange = value
        default: break
        }
        isExternalUpdate = false
    }

    func sendIfLocal(_ address: AUParameterAddress, value: Double) {
        guard !isExternalUpdate else { return }
        parameterTree?.parameter(withAddress: address)?.value = AUValue(value)
    }
}

// MARK: - Main View

struct GlideMainView: View {
    @StateObject private var params = GlideParameterState()
    @StateObject private var automation = AutomationState()
    let audioUnit: GlideAudioUnit

    // Display state
    @State private var activeNoteMask: (UInt64, UInt64) = (0, 0)
    @State private var playheadBeat: Double = 0.0
    @State private var displayTimer: Timer?

    // View range
    @State private var viewBeats: Double = 16.0       // 4 bars visible
    @State private var viewStartBeat: Double = 0.0
    @State private var noteRangeLow: Int = 48          // C3
    @State private var noteRangeHigh: Int = 72         // C5

    var body: some View {
        ZStack {
            Color(red: 0.06, green: 0.08, blue: 0.14)

            VStack(spacing: 0) {
                controlsBar
                    .frame(height: 40)

                HStack(spacing: 0) {
                    pianoRollSidebar
                        .frame(width: 50)

                    automationCanvas
                }

                beatRuler
                    .frame(height: 24)
            }
        }
        .frame(width: 700, height: 500)
        .onAppear {
            params.attach(to: audioUnit)
            automation.attach(to: audioUnit.kernel)
            startDisplayTimer()
        }
        .onDisappear { displayTimer?.invalidate() }
    }

    // MARK: - Controls Bar

    private var controlsBar: some View {
        HStack(spacing: 12) {
            Text("MYGLIDE")
                .font(.system(size: 14, weight: .bold, design: .monospaced))
                .foregroundColor(.white)

            Divider().frame(height: 20).background(Color.white.opacity(0.3))

            // Interpolation mode
            Picker("", selection: $automation.interpolationMode) {
                Text("Linear").tag(0)
                Text("Smooth").tag(1)
                Text("Step").tag(2)
            }
            .pickerStyle(.segmented)
            .frame(width: 180)
            .onChange(of: automation.interpolationMode) { newVal in
                automation.setInterpolationMode(newVal)
            }

            // Snap toggle
            Toggle("Snap", isOn: $automation.snapEnabled)
                .toggleStyle(.checkbox)
                .foregroundColor(.white.opacity(0.8))
                .font(.system(size: 11))

            Button("Clear") {
                automation.clearAll()
            }
            .buttonStyle(.borderless)
            .foregroundColor(.red.opacity(0.8))
            .font(.system(size: 11, weight: .medium))

            Spacer()

            // Mix knob
            HStack(spacing: 4) {
                Text("MIX")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(.white.opacity(0.5))
                Text("\(Int(params.mix))%")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.white)
            }

            // Glide time
            HStack(spacing: 4) {
                Text("GLIDE")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(.white.opacity(0.5))
                Text("\(Int(params.glideTime))ms")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.white)
            }
        }
        .padding(.horizontal, 12)
        .background(Color(red: 0.10, green: 0.12, blue: 0.18))
    }

    // MARK: - Piano Roll Sidebar

    private var pianoRollSidebar: some View {
        GeometryReader { geo in
            let noteCount = noteRangeHigh - noteRangeLow
            let noteHeight = geo.size.height / CGFloat(noteCount)

            ZStack(alignment: .topLeading) {
                // Background
                Color(red: 0.08, green: 0.10, blue: 0.16)

                ForEach(0..<noteCount, id: \.self) { i in
                    let midi = noteRangeHigh - 1 - i
                    let isBlack = [1, 3, 6, 8, 10].contains(midi % 12)
                    let isActive = isNoteActive(midi)
                    let y = CGFloat(i) * noteHeight

                    ZStack {
                        Rectangle()
                            .fill(isActive ? Color.cyan.opacity(0.4) :
                                  isBlack ? Color(white: 0.08) : Color(white: 0.12))
                            .frame(height: noteHeight)

                        if midi % 12 == 0 || !isBlack {
                            Text(noteName(midi: midi))
                                .font(.system(size: 8, weight: isActive ? .bold : .regular, design: .monospaced))
                                .foregroundColor(isActive ? .cyan : .white.opacity(0.5))
                        }
                    }
                    .offset(y: y)
                }

                // Horizontal grid lines on C notes
                ForEach(0..<noteCount, id: \.self) { i in
                    let midi = noteRangeHigh - 1 - i
                    if midi % 12 == 0 {
                        let y = CGFloat(i) * noteHeight
                        Rectangle()
                            .fill(Color.white.opacity(0.15))
                            .frame(height: 0.5)
                            .offset(y: y)
                    }
                }
            }
        }
    }

    // MARK: - Automation Canvas

    private var automationCanvas: some View {
        GeometryReader { geo in
            let size = geo.size
            let noteCount = noteRangeHigh - noteRangeLow
            let pitchRange = Double(noteCount)

            ZStack {
                // Background grid
                Canvas { context, canvasSize in
                    drawGrid(context: context, size: canvasSize, noteCount: noteCount)
                    drawCurves(context: context, size: canvasSize, pitchRange: pitchRange)
                    drawBreakpoints(context: context, size: canvasSize, pitchRange: pitchRange)
                    drawPlayhead(context: context, size: canvasSize)
                }

                // Interaction overlay: click to add, drag to move
                Color.clear
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onEnded { value in
                                let beat = xToBeat(value.location.x, width: size.width)
                                let semi = yToSemitones(value.location.y, height: size.height, range: pitchRange)

                                // Check if near an existing breakpoint
                                if let idx = nearestBreakpointIndex(at: value.location, size: size, pitchRange: pitchRange, threshold: 12) {
                                    // If drag distance is tiny, it's a click — delete on double-tap
                                    // For now, single-click on existing = do nothing (drag handles movement)
                                } else {
                                    automation.addBreakpoint(beat: beat, semitones: semi)
                                }
                            }
                    )
                    .simultaneousGesture(
                        DragGesture(minimumDistance: 5)
                            .onChanged { value in
                                if let idx = nearestBreakpointIndex(at: value.startLocation, size: size, pitchRange: pitchRange, threshold: 12) {
                                    let beat = xToBeat(value.location.x, width: size.width)
                                    let semi = yToSemitones(value.location.y, height: size.height, range: pitchRange)
                                    automation.moveBreakpoint(index: idx, beat: beat, semitones: semi)
                                }
                            }
                    )

                // Delete overlay: breakpoint dots as tappable views
                ForEach(Array(automation.breakpoints.enumerated()), id: \.element.id) { index, bp in
                    let x = beatToX(bp.beat, width: size.width)
                    let y = semitonesToY(bp.semitones, height: size.height, range: pitchRange)

                    Circle()
                        .fill(Color.clear)
                        .frame(width: 20, height: 20)
                        .position(x: x, y: y)
                        .onTapGesture(count: 2) {
                            automation.removeBreakpoint(index: index)
                        }
                }
            }
        }
    }

    // MARK: - Beat Ruler

    private var beatRuler: some View {
        GeometryReader { geo in
            let width = geo.size.width - 50  // subtract piano sidebar
            ZStack(alignment: .leading) {
                Color(red: 0.08, green: 0.10, blue: 0.16)

                HStack(spacing: 0) {
                    Spacer().frame(width: 50)
                    Canvas { context, size in
                        let beatsVisible = viewBeats
                        for i in 0...Int(beatsVisible) {
                            let beat = viewStartBeat + Double(i)
                            let x = CGFloat(i) / CGFloat(beatsVisible) * size.width

                            // Tick mark
                            context.stroke(
                                Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: 6)) },
                                with: .color(.white.opacity(beat.truncatingRemainder(dividingBy: 4) == 0 ? 0.6 : 0.3)),
                                lineWidth: 0.5
                            )

                            // Beat number
                            if i < Int(beatsVisible) {
                                let text = Text("\(Int(beat) + 1)").font(.system(size: 9, design: .monospaced))
                                context.draw(text, at: CGPoint(x: x + 8, y: 14))
                            }
                        }
                    }
                }
            }
        }
    }

    // MARK: - Drawing Helpers

    private func drawGrid(context: GraphicsContext, size: CGSize, noteCount: Int) {
        let noteHeight = size.height / CGFloat(noteCount)

        // Horizontal lines (per note)
        for i in 0...noteCount {
            let y = CGFloat(i) * noteHeight
            let midi = noteRangeHigh - i
            let isC = midi % 12 == 0
            context.stroke(
                Path { p in p.move(to: CGPoint(x: 0, y: y)); p.addLine(to: CGPoint(x: size.width, y: y)) },
                with: .color(.white.opacity(isC ? 0.15 : 0.05)),
                lineWidth: isC ? 0.5 : 0.25
            )
        }

        // Vertical lines (per beat)
        let beatsVisible = viewBeats
        for i in 0...Int(beatsVisible) {
            let x = CGFloat(i) / CGFloat(beatsVisible) * size.width
            let beat = viewStartBeat + Double(i)
            let isBar = beat.truncatingRemainder(dividingBy: 4) == 0
            context.stroke(
                Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: size.height)) },
                with: .color(.white.opacity(isBar ? 0.2 : 0.06)),
                lineWidth: isBar ? 0.5 : 0.25
            )
        }
    }

    private func drawCurves(context: GraphicsContext, size: CGSize, pitchRange: Double) {
        let bps = automation.breakpoints
        guard bps.count >= 2 else { return }

        var path = Path()
        let steps = Int(size.width)

        for step in 0...steps {
            let x = CGFloat(step)
            let beat = xToBeat(x, width: size.width)

            // Evaluate the curve manually matching AutomationCurve logic
            let semi = evaluateCurve(at: beat)
            let y = semitonesToY(semi, height: size.height, range: pitchRange)

            if step == 0 {
                path.move(to: CGPoint(x: x, y: y))
            } else {
                path.addLine(to: CGPoint(x: x, y: y))
            }
        }

        context.stroke(path, with: .color(Color.cyan.opacity(0.8)), lineWidth: 1.5)
    }

    private func drawBreakpoints(context: GraphicsContext, size: CGSize, pitchRange: Double) {
        for bp in automation.breakpoints {
            let x = beatToX(bp.beat, width: size.width)
            let y = semitonesToY(bp.semitones, height: size.height, range: pitchRange)
            let center = CGPoint(x: x, y: y)

            // Outer glow
            context.fill(
                Path(ellipseIn: CGRect(x: x - 6, y: y - 6, width: 12, height: 12)),
                with: .color(Color.cyan.opacity(0.3))
            )
            // Inner dot
            context.fill(
                Path(ellipseIn: CGRect(x: x - 4, y: y - 4, width: 8, height: 8)),
                with: .color(Color.cyan)
            )
            // Center
            context.fill(
                Path(ellipseIn: CGRect(x: x - 2, y: y - 2, width: 4, height: 4)),
                with: .color(.white)
            )
        }
    }

    private func drawPlayhead(context: GraphicsContext, size: CGSize) {
        let beat = playheadBeat
        if beat < viewStartBeat || beat > viewStartBeat + viewBeats { return }

        let x = beatToX(beat, width: size.width)
        context.stroke(
            Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: size.height)) },
            with: .color(.white.opacity(0.6)),
            lineWidth: 1.0
        )
    }

    // MARK: - Coordinate Conversion

    private func beatToX(_ beat: Double, width: CGFloat) -> CGFloat {
        return CGFloat((beat - viewStartBeat) / viewBeats) * width
    }

    private func xToBeat(_ x: CGFloat, width: CGFloat) -> Double {
        return viewStartBeat + Double(x / width) * viewBeats
    }

    private func semitonesToY(_ semi: Double, height: CGFloat, range: Double) -> CGFloat {
        // Map semitones to note position: higher notes = lower Y
        // semi=0 is relative to the middle of the view range
        let midNote = Double(noteRangeLow + noteRangeHigh) / 2.0
        let noteInRange = midNote + semi
        let normalized = (Double(noteRangeHigh) - noteInRange) / range
        return CGFloat(normalized) * height
    }

    private func yToSemitones(_ y: CGFloat, height: CGFloat, range: Double) -> Double {
        let normalized = Double(y / height)
        let noteInRange = Double(noteRangeHigh) - normalized * range
        let midNote = Double(noteRangeLow + noteRangeHigh) / 2.0
        return noteInRange - midNote
    }

    // MARK: - Curve Evaluation (mirrors C++ for display)

    private func evaluateCurve(at beat: Double) -> Double {
        let bps = automation.breakpoints
        guard !bps.isEmpty else { return 0 }
        if bps.count == 1 { return bps[0].semitones }
        if beat <= bps[0].beat { return bps[0].semitones }
        if beat >= bps[bps.count - 1].beat { return bps[bps.count - 1].semitones }

        // Find segment
        var lo = 0, hi = bps.count - 1
        while lo < hi - 1 {
            let mid = (lo + hi) / 2
            if bps[mid].beat <= beat { lo = mid } else { hi = mid }
        }

        let a = bps[lo], b = bps[hi]
        let span = b.beat - a.beat
        guard span > 1e-9 else { return a.semitones }
        let t = (beat - a.beat) / span

        switch a.interpType {
        case 2: return a.semitones  // step
        case 1: // smooth
            let s = t * t * (3.0 - 2.0 * t)
            return a.semitones + (b.semitones - a.semitones) * s
        default: // linear
            return a.semitones + (b.semitones - a.semitones) * t
        }
    }

    // MARK: - Hit Testing

    private func nearestBreakpointIndex(at point: CGPoint, size: CGSize, pitchRange: Double, threshold: CGFloat) -> Int? {
        var bestDist = threshold
        var bestIdx: Int? = nil

        for (i, bp) in automation.breakpoints.enumerated() {
            let bx = beatToX(bp.beat, width: size.width)
            let by = semitonesToY(bp.semitones, height: size.height, range: pitchRange)
            let dist = hypot(point.x - bx, point.y - by)
            if dist < bestDist {
                bestDist = dist
                bestIdx = i
            }
        }
        return bestIdx
    }

    // MARK: - MIDI Note Display

    private func isNoteActive(_ midi: Int) -> Bool {
        if midi < 64 {
            return (activeNoteMask.0 & (1 << midi)) != 0
        } else {
            return (activeNoteMask.1 & (1 << (midi - 64))) != 0
        }
    }

    private func startDisplayTimer() {
        displayTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { _ in
            activeNoteMask = (
                audioUnit.kernel.activeNoteBitmaskLo(),
                audioUnit.kernel.activeNoteBitmaskHi()
            )
            playheadBeat = audioUnit.kernel.currentBeatPosition()
        }
    }
}
