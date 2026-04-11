import SwiftUI
import AVFoundation

// MARK: - Note Helpers

private let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

private func noteName(midi: Int) -> String {
    let name = noteNames[midi % 12]
    let octave = (midi / 12) - 1
    return "\(name)\(octave)"
}

// MARK: - Automation State

/// Bridges the C++ AutomationCurve to SwiftUI via Obj-C++ bridge methods.
/// Includes undo/redo, beat snapping, and preset restore support.
class AutomationState: ObservableObject {
    struct UIBreakpoint: Identifiable {
        let id = UUID()
        var beat: Double
        var semitones: Double
        var interpType: Int  // 0=linear, 1=smooth, 2=step
    }

    @Published var breakpoints: [UIBreakpoint] = []
    @Published var interpolationMode: Int = 0
    @Published var snapEnabled: Bool = true
    @Published var beatSnapDivision: Double = 0.25  // 0=free, 1.0=1/4, 0.5=1/8, 0.25=1/16
    @Published var canUndo: Bool = false
    @Published var canRedo: Bool = false
    @Published var drawMode: Bool = false        // pencil tool: drag to create breakpoints
    @Published var selectedIndex: Int? = nil      // for Delete key

    private weak var kernelBridge: GlideDSPKernelBridge?
    private var undoStack: [[UIBreakpoint]] = []
    private var redoStack: [[UIBreakpoint]] = []
    private let maxUndoLevels = 50
    private var dragUndoPushed = false

    func attach(to bridge: GlideDSPKernelBridge) {
        kernelBridge = bridge
    }

    // MARK: - Beat Snap

    private func snapBeat(_ beat: Double) -> Double {
        guard snapEnabled && beatSnapDivision > 0 else { return beat }
        return (beat / beatSnapDivision).rounded() * beatSnapDivision
    }

    // MARK: - Undo/Redo

    private func pushUndo() {
        undoStack.append(breakpoints)
        if undoStack.count > maxUndoLevels { undoStack.removeFirst() }
        redoStack.removeAll()
        canUndo = true
        canRedo = false
    }

    func undo() {
        guard let previous = undoStack.popLast() else { return }
        redoStack.append(breakpoints)
        breakpoints = previous
        canUndo = !undoStack.isEmpty
        canRedo = true
        commitToDSP()
    }

    func redo() {
        guard let next = redoStack.popLast() else { return }
        undoStack.append(breakpoints)
        breakpoints = next
        canUndo = true
        canRedo = !redoStack.isEmpty
        commitToDSP()
    }

    func beginDrag() {
        if !dragUndoPushed {
            pushUndo()
            dragUndoPushed = true
        }
    }

    func endDrag() {
        dragUndoPushed = false
    }

    /// Draw mode: add a breakpoint along the drag path (called on each onChanged).
    /// Only adds if far enough from the last breakpoint to avoid clutter.
    func drawBreakpoint(beat: Double, semitones: Double) {
        let snappedBeat = snapBeat(beat)
        let snappedSemi = snapEnabled ? semitones.rounded() : semitones

        // Skip if too close to the last breakpoint (min 0.25 beat apart)
        if let last = breakpoints.last, abs(snappedBeat - last.beat) < 0.2 {
            return
        }

        breakpoints.append(UIBreakpoint(beat: snappedBeat, semitones: snappedSemi, interpType: interpolationMode))
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
    }

    func deleteSelected() {
        guard let idx = selectedIndex, idx >= 0, idx < breakpoints.count else { return }
        pushUndo()
        breakpoints.remove(at: idx)
        selectedIndex = nil
        commitToDSP()
    }

    // MARK: - Editing

    func addBreakpoint(beat: Double, semitones: Double) {
        pushUndo()
        let snappedBeat = snapBeat(beat)
        let snappedSemi = snapEnabled ? semitones.rounded() : semitones
        breakpoints.append(UIBreakpoint(beat: snappedBeat, semitones: snappedSemi, interpType: interpolationMode))
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
    }

    func moveBreakpoint(index: Int, beat: Double, semitones: Double) {
        guard index >= 0 && index < breakpoints.count else { return }
        let snappedBeat = snapBeat(beat)
        let snappedSemi = snapEnabled ? semitones.rounded() : semitones
        breakpoints[index].beat = max(0, snappedBeat)
        breakpoints[index].semitones = snappedSemi
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
    }

    func removeBreakpoint(index: Int) {
        guard index >= 0 && index < breakpoints.count else { return }
        pushUndo()
        breakpoints.remove(at: index)
        commitToDSP()
    }

    func clearAll() {
        pushUndo()
        breakpoints.removeAll()
        commitToDSP()
    }

    func setInterpolationMode(_ mode: Int) {
        pushUndo()
        interpolationMode = mode
        for i in breakpoints.indices {
            breakpoints[i].interpType = mode
        }
        commitToDSP()
    }

    // MARK: - Preset Restore

    func restoreFromDSP() {
        guard let bridge = kernelBridge else { return }
        let count = bridge.automationBreakpointCount()
        breakpoints.removeAll()
        for i in 0..<count {
            var beat: Double = 0, semi: Double = 0, interp: UInt8 = 0
            bridge.automationBreakpoint(atIndex: Int32(i), beat: &beat, semitones: &semi, interpType: &interp)
            breakpoints.append(UIBreakpoint(beat: beat, semitones: semi, interpType: Int(interp)))
        }
        undoStack.removeAll()
        redoStack.removeAll()
        canUndo = false
        canRedo = false
    }

    // MARK: - DSP Commit

    private func commitToDSP() {
        guard let bridge = kernelBridge else { return }
        bridge.automationBeginEdit()
        bridge.automationClear()
        for bp in breakpoints {
            bridge.automationAddBreakpoint(atBeat: bp.beat, semitones: bp.semitones, interpType: UInt8(bp.interpType))
        }
        bridge.automationCommitEdit()
    }
}

// MARK: - Parameter State

class GlideParameterState: ObservableObject {
    @Published var glideTime: Double = 50.0
    @Published var mix: Double = 100.0
    @Published var pitchRange: Double = 24.0
    @Published var pitchOffset: Double = 0.0

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
        case 3: pitchOffset = value
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
    @State private var currentPitch: Double = 0.0
    @State private var displayTimer: Timer?

    // View range (zoom/scroll)
    @State private var viewBeats: Double = 16.0
    @State private var viewStartBeat: Double = 0.0
    @State private var baseViewBeats: Double = 16.0    // for pinch gesture
    @State private var basePanStart: Double = 0.0      // for pan gesture
    @State private var noteRangeLow: Int = 48
    @State private var noteRangeHigh: Int = 72

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
        .frame(minWidth: 500, idealWidth: 700, minHeight: 350, idealHeight: 500)
        .background(
            // Hidden buttons for keyboard shortcuts
            Group {
                Button("") { automation.undo() }
                    .keyboardShortcut("z", modifiers: .command)
                Button("") { automation.redo() }
                    .keyboardShortcut("z", modifiers: [.command, .shift])
                Button("") { automation.deleteSelected() }
                    .keyboardShortcut(.delete, modifiers: [])
            }
            .frame(width: 0, height: 0)
            .opacity(0)
        )
        .onAppear {
            params.attach(to: audioUnit)
            automation.attach(to: audioUnit.kernel)
            audioUnit.onStateRestored = { [weak automation] in
                automation?.restoreFromDSP()
            }
            startDisplayTimer()
        }
        .onDisappear { displayTimer?.invalidate() }
    }

    // MARK: - Controls Bar

    private var controlsBar: some View {
        HStack(spacing: 8) {
            Text("MYGLIDE")
                .font(.system(size: 14, weight: .bold, design: .monospaced))
                .foregroundColor(.white)

            Divider().frame(height: 20).background(Color.white.opacity(0.3))

            // Undo/Redo
            Button(action: { automation.undo() }) {
                Image(systemName: "arrow.uturn.backward")
                    .font(.system(size: 11))
            }
            .buttonStyle(.borderless)
            .foregroundColor(automation.canUndo ? .white : .white.opacity(0.2))
            .disabled(!automation.canUndo)

            Button(action: { automation.redo() }) {
                Image(systemName: "arrow.uturn.forward")
                    .font(.system(size: 11))
            }
            .buttonStyle(.borderless)
            .foregroundColor(automation.canRedo ? .white : .white.opacity(0.2))
            .disabled(!automation.canRedo)

            Divider().frame(height: 20).background(Color.white.opacity(0.3))

            // Interpolation mode
            Picker("", selection: $automation.interpolationMode) {
                Text("Linear").tag(0)
                Text("Smooth").tag(1)
                Text("Step").tag(2)
            }
            .pickerStyle(.segmented)
            .frame(width: 160)
            .onChange(of: automation.interpolationMode) { newVal in
                automation.setInterpolationMode(newVal)
            }

            // Snap + beat grid
            Toggle("Snap", isOn: $automation.snapEnabled)
                .toggleStyle(.checkbox)
                .foregroundColor(.white.opacity(0.8))
                .font(.system(size: 11))

            if automation.snapEnabled {
                Picker("", selection: $automation.beatSnapDivision) {
                    Text("1/4").tag(1.0)
                    Text("1/8").tag(0.5)
                    Text("1/16").tag(0.25)
                }
                .pickerStyle(.segmented)
                .frame(width: 100)
            }

            // Draw mode toggle (pencil tool)
            Toggle(isOn: $automation.drawMode) {
                Image(systemName: "pencil.line")
                    .font(.system(size: 11))
            }
            .toggleStyle(.button)
            .foregroundColor(automation.drawMode ? .cyan : .white.opacity(0.6))
            .help("Draw mode: drag to create breakpoints")

            Button("Clear") { automation.clearAll() }
                .buttonStyle(.borderless)
                .foregroundColor(.red.opacity(0.8))
                .font(.system(size: 11, weight: .medium))

            Spacer()

            // Pitch indicator
            HStack(spacing: 4) {
                Text("PITCH")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(.white.opacity(0.5))
                Text(String(format: "%+.1f st", currentPitch))
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(abs(currentPitch) < 0.1 ? .white.opacity(0.5) : .cyan)
            }

            if abs(params.pitchOffset) > 0.1 {
                HStack(spacing: 4) {
                    Text("OFFSET")
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundColor(.white.opacity(0.5))
                    Text(String(format: "%+.1f", params.pitchOffset))
                        .font(.system(size: 11, weight: .medium, design: .monospaced))
                        .foregroundColor(.orange)
                }
            }

            HStack(spacing: 4) {
                Text("MIX")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(.white.opacity(0.5))
                Text("\(Int(params.mix))%")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.white)
            }

            HStack(spacing: 4) {
                Text("GLIDE")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundColor(.white.opacity(0.5))
                Text("\(Int(params.glideTime))ms")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundColor(.white)
            }
        }
        .padding(.horizontal, 8)
        .background(Color(red: 0.10, green: 0.12, blue: 0.18))
    }

    // MARK: - Piano Roll Sidebar

    private var pianoRollSidebar: some View {
        GeometryReader { geo in
            let noteCount = noteRangeHigh - noteRangeLow
            let noteHeight = geo.size.height / CGFloat(noteCount)

            ZStack(alignment: .topLeading) {
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
                Canvas { context, canvasSize in
                    drawGrid(context: context, size: canvasSize, noteCount: noteCount)
                    drawCurves(context: context, size: canvasSize, pitchRange: pitchRange)
                    drawBreakpoints(context: context, size: canvasSize, pitchRange: pitchRange)
                    drawPlayhead(context: context, size: canvasSize, pitchRange: pitchRange)
                }

                // Interaction layer: click to add/select, drag to move or draw
                Color.clear
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onEnded { value in
                                let beat = xToBeat(value.location.x, width: size.width)
                                let semi = yToSemitones(value.location.y, height: size.height, range: pitchRange)

                                if let idx = nearestBreakpointIndex(at: value.location, size: size, pitchRange: pitchRange, threshold: 12) {
                                    automation.selectedIndex = idx
                                } else if !automation.drawMode {
                                    automation.addBreakpoint(beat: beat, semitones: semi)
                                }
                            }
                    )
                    .simultaneousGesture(
                        DragGesture(minimumDistance: 5)
                            .onChanged { value in
                                let beat = xToBeat(value.location.x, width: size.width)
                                let semi = yToSemitones(value.location.y, height: size.height, range: pitchRange)

                                if automation.drawMode {
                                    // Draw mode: create breakpoints along the drag path
                                    automation.beginDrag()
                                    automation.drawBreakpoint(beat: beat, semitones: semi)
                                } else if let idx = nearestBreakpointIndex(at: value.startLocation, size: size, pitchRange: pitchRange, threshold: 12) {
                                    automation.beginDrag()
                                    automation.moveBreakpoint(index: idx, beat: beat, semitones: semi)
                                }
                            }
                            .onEnded { _ in
                                automation.endDrag()
                            }
                    )

                // Pinch-to-zoom
                Color.clear
                    .contentShape(Rectangle())
                    .gesture(
                        MagnifyGesture()
                            .onChanged { value in
                                let newBeats = baseViewBeats / value.magnification
                                viewBeats = max(4.0, min(64.0, newBeats))
                            }
                            .onEnded { _ in
                                baseViewBeats = viewBeats
                            }
                    )

                // Double-click to delete
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

    // MARK: - Beat Ruler (drag to pan)

    private var beatRuler: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Color(red: 0.08, green: 0.10, blue: 0.16)

                HStack(spacing: 0) {
                    Spacer().frame(width: 50)
                    Canvas { context, size in
                        // Adaptive tick density based on zoom level
                        let tickInterval: Double
                        if viewBeats <= 8 { tickInterval = 0.25 }
                        else if viewBeats <= 16 { tickInterval = 0.5 }
                        else if viewBeats <= 32 { tickInterval = 1.0 }
                        else { tickInterval = 2.0 }

                        let firstTick = (viewStartBeat / tickInterval).rounded(.up) * tickInterval
                        var tick = firstTick
                        while tick <= viewStartBeat + viewBeats {
                            let x = beatToX(tick, width: size.width)
                            let isBar = tick.truncatingRemainder(dividingBy: 4) == 0
                            let isBeat = tick.truncatingRemainder(dividingBy: 1) == 0

                            let height: CGFloat = isBar ? 8 : (isBeat ? 5 : 3)
                            context.stroke(
                                Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: height)) },
                                with: .color(.white.opacity(isBar ? 0.6 : 0.3)),
                                lineWidth: 0.5
                            )

                            if isBeat {
                                let text = Text("\(Int(tick) + 1)").font(.system(size: 9, design: .monospaced))
                                context.draw(text, at: CGPoint(x: x + 8, y: 16))
                            }

                            tick += tickInterval
                        }
                    }
                    .gesture(
                        DragGesture()
                            .onChanged { value in
                                let beatDelta = Double(value.translation.width / (geo.size.width - 50)) * viewBeats
                                viewStartBeat = max(0, basePanStart - beatDelta)
                            }
                            .onEnded { _ in
                                basePanStart = viewStartBeat
                            }
                    )
                }
            }
        }
    }

    // MARK: - Drawing Helpers

    private func drawGrid(context: GraphicsContext, size: CGSize, noteCount: Int) {
        let noteHeight = size.height / CGFloat(noteCount)

        // Horizontal note lines
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

        // Adaptive vertical beat grid
        let tickInterval: Double
        if viewBeats <= 8 { tickInterval = 0.25 }
        else if viewBeats <= 16 { tickInterval = 0.5 }
        else if viewBeats <= 32 { tickInterval = 1.0 }
        else { tickInterval = 2.0 }

        let firstTick = (viewStartBeat / tickInterval).rounded(.up) * tickInterval
        var tick = firstTick
        while tick <= viewStartBeat + viewBeats {
            let x = beatToX(tick, width: size.width)
            let isBar = tick.truncatingRemainder(dividingBy: 4) == 0
            let isBeat = tick.truncatingRemainder(dividingBy: 1) == 0
            context.stroke(
                Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: size.height)) },
                with: .color(.white.opacity(isBar ? 0.2 : (isBeat ? 0.08 : 0.04))),
                lineWidth: isBar ? 0.5 : 0.25
            )
            tick += tickInterval
        }

        // Beat snap grid overlay
        if automation.snapEnabled && automation.beatSnapDivision > 0 && automation.beatSnapDivision < tickInterval {
            let snap = automation.beatSnapDivision
            let firstSnap = (viewStartBeat / snap).rounded(.up) * snap
            var snapBeat = firstSnap
            while snapBeat <= viewStartBeat + viewBeats {
                let x = beatToX(snapBeat, width: size.width)
                context.stroke(
                    Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: size.height)) },
                    with: .color(Color.cyan.opacity(0.06)),
                    lineWidth: 0.25
                )
                snapBeat += snap
            }
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
            let semi = evaluateCurve(at: beat)
            let y = semitonesToY(semi, height: size.height, range: pitchRange)

            if step == 0 { path.move(to: CGPoint(x: x, y: y)) }
            else { path.addLine(to: CGPoint(x: x, y: y)) }
        }

        context.stroke(path, with: .color(Color.cyan.opacity(0.8)), lineWidth: 1.5)
    }

    private func drawBreakpoints(context: GraphicsContext, size: CGSize, pitchRange: Double) {
        for (i, bp) in automation.breakpoints.enumerated() {
            let x = beatToX(bp.beat, width: size.width)
            let y = semitonesToY(bp.semitones, height: size.height, range: pitchRange)
            let isSelected = automation.selectedIndex == i

            // Selection ring
            if isSelected {
                context.stroke(
                    Path(ellipseIn: CGRect(x: x - 8, y: y - 8, width: 16, height: 16)),
                    with: .color(.white),
                    lineWidth: 1.5
                )
            }

            context.fill(
                Path(ellipseIn: CGRect(x: x - 6, y: y - 6, width: 12, height: 12)),
                with: .color(Color.cyan.opacity(isSelected ? 0.5 : 0.3))
            )
            context.fill(
                Path(ellipseIn: CGRect(x: x - 4, y: y - 4, width: 8, height: 8)),
                with: .color(Color.cyan)
            )
            context.fill(
                Path(ellipseIn: CGRect(x: x - 2, y: y - 2, width: 4, height: 4)),
                with: .color(.white)
            )
        }
    }

    private func drawPlayhead(context: GraphicsContext, size: CGSize, pitchRange: Double) {
        let beat = playheadBeat
        if beat < viewStartBeat || beat > viewStartBeat + viewBeats { return }

        let x = beatToX(beat, width: size.width)

        // Playhead line
        context.stroke(
            Path { p in p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: size.height)) },
            with: .color(.white.opacity(0.6)),
            lineWidth: 1.0
        )

        // Pitch dot on the curve at playhead position
        if abs(currentPitch) > 0.01 {
            let pitchY = semitonesToY(currentPitch, height: size.height, range: pitchRange)
            context.fill(
                Path(ellipseIn: CGRect(x: x - 4, y: pitchY - 4, width: 8, height: 8)),
                with: .color(.white)
            )
        }
    }

    // MARK: - Coordinate Conversion

    private func beatToX(_ beat: Double, width: CGFloat) -> CGFloat {
        CGFloat((beat - viewStartBeat) / viewBeats) * width
    }

    private func xToBeat(_ x: CGFloat, width: CGFloat) -> Double {
        viewStartBeat + Double(x / width) * viewBeats
    }

    private func semitonesToY(_ semi: Double, height: CGFloat, range: Double) -> CGFloat {
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
        case 2: return a.semitones
        case 1:
            let s = t * t * (3.0 - 2.0 * t)
            return a.semitones + (b.semitones - a.semitones) * s
        default:
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
            return (activeNoteMask.0 & (UInt64(1) << midi)) != 0
        } else {
            return (activeNoteMask.1 & (UInt64(1) << (midi - 64))) != 0
        }
    }

    private func startDisplayTimer() {
        displayTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { _ in
            activeNoteMask = (
                audioUnit.kernel.activeNoteBitmaskLo(),
                audioUnit.kernel.activeNoteBitmaskHi()
            )
            playheadBeat = audioUnit.kernel.currentBeatPosition()
            currentPitch = audioUnit.kernel.currentPitchSemitones()
        }
    }
}
