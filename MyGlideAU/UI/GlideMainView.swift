import SwiftUI
import AVFoundation

// MARK: - Note Helpers

private let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

private func noteName(midi: Int) -> String {
    let name = noteNames[midi % 12]
    let octave = (midi / 12) - 1
    return "\(name)\(octave)"
}

/// Helpers for classifying MIDI notes as black/white piano keys.
private enum PianoKey {
    private static let blackPitchClasses: Set<Int> = [1, 3, 6, 8, 10]

    static func isBlack(midi: Int) -> Bool {
        blackPitchClasses.contains(((midi % 12) + 12) % 12)
    }
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

    func duplicateBreakpoint(index: Int) {
        guard index >= 0 && index < breakpoints.count else { return }
        pushUndo()
        let src = breakpoints[index]
        let newBeat = src.beat + 0.5
        breakpoints.append(UIBreakpoint(beat: newBeat, semitones: src.semitones, interpType: src.interpType))
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
    }

    func setBreakpointInterp(index: Int, interpType: Int) {
        guard index >= 0 && index < breakpoints.count else { return }
        pushUndo()
        breakpoints[index].interpType = interpType
        commitToDSP()
    }

    func setBreakpointValue(index: Int, beat: Double, semitones: Double) {
        guard index >= 0 && index < breakpoints.count else { return }
        pushUndo()
        breakpoints[index].beat = max(0, beat)
        breakpoints[index].semitones = semitones
        breakpoints.sort { $0.beat < $1.beat }
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
            bridge.automationBreakpoint(at: Int32(i), beat: &beat, semitones: &semi, interpType: &interp)
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
    @Published var shifterMode: Double = 0.0   // 0=Granular, 1=Vocoder
    @Published var autoGlide: Double = 0.0     // 0=Manual, 1=Auto
    @Published var loopEnabled: Double = 0.0   // 0=off, 1=on
    @Published var loopBeats: Double = 16.0    // loop length in beats

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
        case 4: shifterMode = value
        case 5: autoGlide = value
        case 6: loopEnabled = value
        case 7: loopBeats = value
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
    @State private var inputLevel: (Double, Double) = (0, 0)
    @State private var outputLevel: (Double, Double) = (0, 0)
    @State private var autoGlideTarget: Double = 0.0
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

                // Input/output level meters
                HStack(spacing: 0) {
                    Spacer().frame(width: 50)
                    VStack(spacing: 1) {
                        LevelMeterBar(level: inputLevel.0, label: "IN L")
                        LevelMeterBar(level: inputLevel.1, label: "IN R")
                        LevelMeterBar(level: outputLevel.0, label: "OUT L")
                        LevelMeterBar(level: outputLevel.1, label: "OUT R")
                    }
                    .padding(.horizontal, 4)
                }
                .frame(height: 22)
                .background(Color(red: 0.06, green: 0.08, blue: 0.14))

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

            // Shifter mode (Granular / Vocoder)
            Picker("", selection: Binding(
                get: { Int(params.shifterMode) },
                set: { params.sendIfLocal(4, value: Double($0)) }
            )) {
                Text("Granular").tag(0)
                Text("Vocoder").tag(1)
            }
            .pickerStyle(.segmented)
            .frame(width: 140)

            // Auto-glide mode (Manual / Auto)
            Picker("", selection: Binding(
                get: { Int(params.autoGlide) },
                set: { params.sendIfLocal(5, value: Double($0)) }
            )) {
                Text("Manual").tag(0)
                Text("Auto").tag(1)
            }
            .pickerStyle(.segmented)
            .frame(width: 120)

            // Loop toggle + length
            Toggle(isOn: Binding(
                get: { params.loopEnabled > 0.5 },
                set: { params.sendIfLocal(6, value: $0 ? 1.0 : 0.0) }
            )) {
                Image(systemName: "repeat")
                    .font(.system(size: 11))
            }
            .toggleStyle(.button)
            .foregroundColor(params.loopEnabled > 0.5 ? .green : .white.opacity(0.4))
            .help("Loop automation curve")

            if params.loopEnabled > 0.5 {
                Picker("", selection: Binding(
                    get: { Int(params.loopBeats) },
                    set: { params.sendIfLocal(7, value: Double($0)) }
                )) {
                    Text("4").tag(4)
                    Text("8").tag(8)
                    Text("16").tag(16)
                    Text("32").tag(32)
                }
                .pickerStyle(.segmented)
                .frame(width: 100)
            }

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
            let semitoneHeight = geo.size.height / CGFloat(noteCount)
            let width = geo.size.width

            ZStack(alignment: .topLeading) {
                // Dark backdrop that shows through between keys.
                Color(red: 0.03, green: 0.04, blue: 0.06)

                // White-key layer: drawn first so black keys sit on top.
                ForEach(0..<noteCount, id: \.self) { i in
                    let midi = noteRangeHigh - 1 - i
                    if !PianoKey.isBlack(midi: midi) {
                        pianoWhiteKey(
                            midi: midi,
                            yOffset: CGFloat(i) * semitoneHeight,
                            height: semitoneHeight,
                            width: width,
                            isActive: isNoteActive(midi)
                        )
                    }
                }

                // Black-key layer on top, narrower and offset to the left.
                ForEach(0..<noteCount, id: \.self) { i in
                    let midi = noteRangeHigh - 1 - i
                    if PianoKey.isBlack(midi: midi) {
                        pianoBlackKey(
                            midi: midi,
                            yOffset: CGFloat(i) * semitoneHeight,
                            height: semitoneHeight,
                            width: width,
                            isActive: isNoteActive(midi)
                        )
                    }
                }
            }
        }
    }

    @ViewBuilder
    private func pianoWhiteKey(midi: Int, yOffset: CGFloat, height: CGFloat, width: CGFloat, isActive: Bool) -> some View {
        let inactiveFill = LinearGradient(
            colors: [Color(white: 0.96), Color(white: 0.82)],
            startPoint: .leading, endPoint: .trailing
        )
        let activeFill = LinearGradient(
            colors: [
                Color(red: 0.62, green: 0.90, blue: 1.00),
                Color(red: 0.38, green: 0.70, blue: 0.92),
            ],
            startPoint: .leading, endPoint: .trailing
        )

        ZStack(alignment: .trailing) {
            // Key body
            Rectangle()
                .fill(isActive ? AnyShapeStyle(activeFill) : AnyShapeStyle(inactiveFill))

            // Thin divider at the bottom of each white key (between adjacent whites)
            VStack {
                Spacer()
                Rectangle()
                    .fill(Color.black.opacity(0.28))
                    .frame(height: 0.5)
            }

            // Label on C notes
            if midi % 12 == 0 {
                Text(octaveLabel(midi: midi))
                    .font(.system(size: 9, weight: .semibold, design: .rounded))
                    .foregroundColor(Color(white: 0.22))
                    .padding(.trailing, 6)
            }
        }
        .frame(width: width, height: height)
        .scaleEffect(x: isActive ? 0.985 : 1.0, y: isActive ? 0.90 : 1.0, anchor: .leading)
        .brightness(isActive ? -0.05 : 0)
        .shadow(color: .black.opacity(isActive ? 0.35 : 0), radius: 1, x: 0, y: 0)
        .animation(.easeOut(duration: 0.08), value: isActive)
        .offset(y: yOffset)
    }

    @ViewBuilder
    private func pianoBlackKey(midi: Int, yOffset: CGFloat, height: CGFloat, width: CGFloat, isActive: Bool) -> some View {
        let inactiveFill = LinearGradient(
            colors: [Color(white: 0.08), Color(white: 0.22), Color(white: 0.12)],
            startPoint: .top, endPoint: .bottom
        )
        let activeFill = LinearGradient(
            colors: [
                Color(red: 0.10, green: 0.28, blue: 0.40),
                Color(red: 0.18, green: 0.46, blue: 0.60),
                Color(red: 0.08, green: 0.22, blue: 0.34),
            ],
            startPoint: .top, endPoint: .bottom
        )

        let keyWidth = width * 0.58
        let keyHeightBase = max(height - 1, 1)

        RoundedRectangle(cornerRadius: 1.5, style: .continuous)
            .fill(isActive ? AnyShapeStyle(activeFill) : AnyShapeStyle(inactiveFill))
            .overlay(
                // Highlight ridge across the top for subtle 3D look
                RoundedRectangle(cornerRadius: 1.5, style: .continuous)
                    .stroke(Color.white.opacity(isActive ? 0.10 : 0.18), lineWidth: 0.5)
            )
            .frame(width: keyWidth, height: keyHeightBase)
            .scaleEffect(x: isActive ? 0.96 : 1.0, y: isActive ? 0.88 : 1.0, anchor: .leading)
            .brightness(isActive ? -0.04 : 0)
            .shadow(color: .black.opacity(isActive ? 0.5 : 0.35), radius: isActive ? 0.8 : 1.2, x: 0, y: 0.5)
            .animation(.easeOut(duration: 0.08), value: isActive)
            .offset(x: 0, y: yOffset + 0.5)
    }

    private func octaveLabel(midi: Int) -> String {
        // MIDI note 60 == C4 in the "scientific" octave convention used by Logic.
        let octave = (midi / 12) - 1
        return "C\(octave)"
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
                    drawLoopRegion(context: context, size: canvasSize)
                    drawAutoGlideTarget(context: context, size: canvasSize, pitchRange: pitchRange)
                }

                // Interaction layer: click to add/select, drag to move or draw
                // Disabled in Auto mode (MIDI drives pitch, not breakpoints)
                Color.clear
                    .contentShape(Rectangle())
                    .allowsHitTesting(params.autoGlide < 0.5)
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
                        MagnificationGesture()
                            .onChanged { value in
                                let newBeats = baseViewBeats / Double(value)
                                viewBeats = max(4.0, min(64.0, newBeats))
                            }
                            .onEnded { _ in
                                baseViewBeats = viewBeats
                            }
                    )

                // Breakpoint hit targets: double-click to delete, right-click for context menu
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
                        .contextMenu {
                            // Per-breakpoint interpolation type
                            Menu("Interpolation") {
                                Button("Linear") { automation.setBreakpointInterp(index: index, interpType: 0) }
                                Button("Smooth") { automation.setBreakpointInterp(index: index, interpType: 1) }
                                Button("Step")   { automation.setBreakpointInterp(index: index, interpType: 2) }
                            }
                            Divider()
                            Button("Duplicate") { automation.duplicateBreakpoint(index: index) }
                            Button("Delete", role: .destructive) { automation.removeBreakpoint(index: index) }
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

    private func drawLoopRegion(context: GraphicsContext, size: CGSize) {
        guard params.loopEnabled > 0.5 && params.loopBeats > 0 else { return }

        let loopEnd = params.loopBeats
        let xEnd = beatToX(loopEnd, width: size.width)
        let xStart = beatToX(0.0, width: size.width)

        // Draw loop region boundary line
        if xEnd > 0 && xEnd < size.width {
            context.stroke(
                Path { p in p.move(to: CGPoint(x: xEnd, y: 0)); p.addLine(to: CGPoint(x: xEnd, y: size.height)) },
                with: .color(Color.green.opacity(0.5)),
                style: StrokeStyle(lineWidth: 2, dash: [8, 4])
            )

            // "LOOP" label at the boundary
            let text = Text("\(Int(loopEnd))").font(.system(size: 9, weight: .bold, design: .monospaced))
            context.draw(text, at: CGPoint(x: xEnd - 12, y: 12))
        }

        // Dim the area outside the loop region
        if xEnd < size.width {
            context.fill(
                Path(CGRect(x: xEnd, y: 0, width: size.width - xEnd, height: size.height)),
                with: .color(Color.black.opacity(0.3))
            )
        }
    }

    private func drawAutoGlideTarget(context: GraphicsContext, size: CGSize, pitchRange: Double) {
        guard params.autoGlide > 0.5 else { return }

        let y = semitonesToY(autoGlideTarget, height: size.height, range: pitchRange)

        // Horizontal target line across the canvas
        context.stroke(
            Path { p in p.move(to: CGPoint(x: 0, y: y)); p.addLine(to: CGPoint(x: size.width, y: y)) },
            with: .color(Color.orange.opacity(0.5)),
            style: StrokeStyle(lineWidth: 1.5, dash: [6, 4])
        )

        // Target note label
        let noteNum = Int(autoGlideTarget.rounded()) + 60  // relative to C4
        let noteName = noteNames[((noteNum % 12) + 12) % 12]
        let octave = (noteNum / 12) - 1
        let label = "\(noteName)\(octave)"
        let text = Text(label).font(.system(size: 10, weight: .bold, design: .monospaced))
        context.draw(text, at: CGPoint(x: size.width - 30, y: y - 10))
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
            inputLevel = (audioUnit.kernel.inputLevelL(), audioUnit.kernel.inputLevelR())
            outputLevel = (audioUnit.kernel.outputLevelL(), audioUnit.kernel.outputLevelR())
            autoGlideTarget = audioUnit.kernel.autoGlideTarget()
        }
    }
}

// MARK: - Level Meter Bar

private struct LevelMeterBar: View {
    let level: Double
    let label: String

    private var clampedLevel: Double { min(1.0, max(0.0, level)) }

    private var meterColor: Color {
        if level > 0.9 { return Color(red: 0.95, green: 0.3, blue: 0.2) }
        if level > 0.6 { return Color(red: 1.0, green: 0.70, blue: 0.25) }
        return Color(red: 0.3, green: 0.8, blue: 0.4)
    }

    var body: some View {
        HStack(spacing: 3) {
            Text(label)
                .font(.system(size: 7, weight: .medium, design: .monospaced))
                .foregroundColor(.white.opacity(0.3))
                .frame(width: 18, alignment: .trailing)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 1.5)
                        .fill(Color.white.opacity(0.06))
                    RoundedRectangle(cornerRadius: 1.5)
                        .fill(meterColor.opacity(0.8))
                        .frame(width: geo.size.width * clampedLevel)
                }
            }
        }
    }
}
