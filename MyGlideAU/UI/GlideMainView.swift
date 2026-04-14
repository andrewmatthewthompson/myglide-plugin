import SwiftUI
import AVFoundation

// MARK: - Note Helpers

private let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

private func noteName(midi: Int) -> String {
    let name = noteNames[midi % 12]
    let octave = (midi / 12) - 1
    return "\(name)\(octave)"
}

/// Small visual-hold for MIDI notes driving the piano sidebar.
///
/// The DSP kernel's note bitmask reflects the real held-note state *right
/// now*. Polling it on the display timer means a MIDI note shorter than
/// one poll interval can be entirely missed, and even notes that are
/// caught flash on for a single frame which is hard to see. This class
/// tracks the last time each note was observed active and keeps it in
/// the display mask for a short hold window after release, so brief
/// MIDI changes still show up on the keyboard reliably.
private final class NoteDisplayLatch {
    /// How long a note stays visible on the keyboard after it has been
    /// released. 120 ms is long enough to see a flash without smearing
    /// rapid sequences together.
    private let holdSeconds: Double = 0.120

    /// Sentinel meaning "never seen". Any real CFAbsoluteTime is > 0.
    private static let kNeverSeen: Double = -1e9
    private var lastSeen: [Double] = Array(repeating: NoteDisplayLatch.kNeverSeen, count: 128)

    /// Refresh the latch from a raw DSP bitmask captured at time `now`
    /// (expected to be `CFAbsoluteTimeGetCurrent()`), and return the
    /// display bitmask (real held notes ∪ notes still inside the hold
    /// window).
    func update(rawMaskLo: UInt64, rawMaskHi: UInt64, now: Double) -> (UInt64, UInt64) {
        // Touch every currently-held note so its "last seen" is fresh.
        var lo = rawMaskLo
        while lo != 0 {
            let bit = lo.trailingZeroBitCount
            lastSeen[bit] = now
            lo &= lo - 1
        }
        var hi = rawMaskHi
        while hi != 0 {
            let bit = hi.trailingZeroBitCount
            lastSeen[64 + bit] = now
            hi &= hi - 1
        }

        let threshold = now - holdSeconds
        var dispLo: UInt64 = 0
        var dispHi: UInt64 = 0
        for i in 0..<64 where lastSeen[i] > threshold {
            dispLo |= UInt64(1) << i
        }
        for i in 0..<64 where lastSeen[64 + i] > threshold {
            dispHi |= UInt64(1) << i
        }
        return (dispLo, dispHi)
    }
}

/// Helpers for classifying MIDI notes as black/white piano keys and
/// checking active-note state from the DSP's 128-bit note bitmask.
private enum PianoKey {
    private static let blackPitchClasses: Set<Int> = [1, 3, 6, 8, 10]

    static func isBlack(midi: Int) -> Bool {
        blackPitchClasses.contains(((midi % 12) + 12) % 12)
    }

    static func isActive(midi: Int, maskLo: UInt64, maskHi: UInt64) -> Bool {
        guard midi >= 0 && midi < 128 else { return false }
        if midi < 64 {
            return (maskLo & (UInt64(1) << midi)) != 0
        } else {
            return (maskHi & (UInt64(1) << (midi - 64))) != 0
        }
    }

    static func octaveLabel(midi: Int) -> String {
        // MIDI note 60 == C4 in the "scientific" octave convention Logic uses.
        let octave = (midi / 12) - 1
        return "C\(octave)"
    }
}

// MARK: - Coordinate geometry
//
// Pulled out of GlideMainView so the rendering subviews below don't need
// to reach back into it. `CurveGeometry` is a pure value type so the
// whole chain stays Equatable-friendly.
private struct CurveGeometry: Equatable {
    var viewStartBeat: Double
    var viewBeats: Double
    var noteRangeLow: Int
    var noteRangeHigh: Int

    func beatToX(_ beat: Double, width: CGFloat) -> CGFloat {
        CGFloat((beat - viewStartBeat) / viewBeats) * width
    }

    func xToBeat(_ x: CGFloat, width: CGFloat) -> Double {
        viewStartBeat + Double(x / width) * viewBeats
    }

    func semitonesToY(_ semi: Double, height: CGFloat) -> CGFloat {
        let midNote = Double(noteRangeLow + noteRangeHigh) / 2.0
        let range = Double(noteRangeHigh - noteRangeLow)
        let noteInRange = midNote + semi
        let normalized = (Double(noteRangeHigh) - noteInRange) / range
        return CGFloat(normalized) * height
    }

    func yToSemitones(_ y: CGFloat, height: CGFloat) -> Double {
        let range = Double(noteRangeHigh - noteRangeLow)
        let normalized = Double(y / height)
        let noteInRange = Double(noteRangeHigh) - normalized * range
        let midNote = Double(noteRangeLow + noteRangeHigh) / 2.0
        return noteInRange - midNote
    }
}

/// Curve evaluator extracted as a free function so both the main view's
/// gesture handling and the extracted rendering subviews can use it
/// without reaching into the view hierarchy.
private func evaluateCurveValue(bps: [AutomationState.UIBreakpoint], at beat: Double) -> Double {
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

// MARK: - Piano Sidebar (extracted subview)
//
// Kept as its own `Equatable` SwiftUI struct so the 30 Hz display timer
// on GlideMainView — which mutates playhead beat / levels / pitch — can
// re-run GlideMainView.body without rebuilding the piano Canvas. SwiftUI's
// diffing compares this struct's stored properties and short-circuits
// when they haven't changed, which is the common case.
private struct PianoSidebarView: View, Equatable {
    let maskLo: UInt64
    let maskHi: UInt64
    let noteRangeLow: Int
    let noteRangeHigh: Int

    var body: some View {
        Canvas(opaque: true, rendersAsynchronously: false) { context, size in
            draw(context: &context, size: size)
        }
    }

    private func draw(context: inout GraphicsContext, size: CGSize) {
        let noteCount = noteRangeHigh - noteRangeLow
        guard noteCount > 0 else { return }

        let semitoneHeight = size.height / CGFloat(noteCount)
        let width = size.width

        // Flat palette — no gradients / shadows.
        let whiteFill          = Color(white: 0.92)
        let whiteActiveFill    = Color(red: 0.55, green: 0.88, blue: 1.00)
        let blackFill          = Color.black
        let blackActiveFill    = Color(red: 0.14, green: 0.42, blue: 0.60)
        let dividerColor       = Color.black.opacity(0.30)
        let labelColor         = Color(white: 0.22)

        let cornerRadius: CGFloat = 3
        let keyInsetY: CGFloat = 0.5
        let blackKeyWidth = width * 0.58
        let blackHeightFraction: CGFloat = 0.68

        // Backdrop
        context.fill(
            Path(CGRect(origin: .zero, size: size)),
            with: .color(Color(red: 0.03, green: 0.04, blue: 0.06))
        )

        // White keys
        for i in 0..<noteCount {
            let midi = noteRangeHigh - 1 - i
            if PianoKey.isBlack(midi: midi) { continue }

            let active = PianoKey.isActive(midi: midi, maskLo: maskLo, maskHi: maskHi)
            let y = CGFloat(i) * semitoneHeight + keyInsetY
            let h = max(semitoneHeight - (keyInsetY * 2), 1)
            let rect = CGRect(x: 0, y: y, width: width, height: h)
            let path = Path(roundedRect: rect, cornerRadius: cornerRadius, style: .continuous)
            context.fill(path, with: .color(active ? whiteActiveFill : whiteFill))

            if midi % 12 == 0 {
                let label = Text(PianoKey.octaveLabel(midi: midi))
                    .font(.system(size: 9, weight: .semibold, design: .rounded))
                    .foregroundColor(labelColor)
                context.draw(label, at: CGPoint(x: width - 6, y: y + h / 2), anchor: .trailing)
            }
        }

        // Divider lines between adjacent white keys, in a single stroke.
        let dividerPath = Path { p in
            for i in 0..<noteCount {
                let midi = noteRangeHigh - 1 - i
                if PianoKey.isBlack(midi: midi) { continue }
                let y = CGFloat(i + 1) * semitoneHeight
                p.move(to: CGPoint(x: 0, y: y))
                p.addLine(to: CGPoint(x: width, y: y))
            }
        }
        context.stroke(dividerPath, with: .color(dividerColor), lineWidth: 0.5)

        // Black keys (shorter than white keys, centred in their row).
        for i in 0..<noteCount {
            let midi = noteRangeHigh - 1 - i
            if !PianoKey.isBlack(midi: midi) { continue }

            let active = PianoKey.isActive(midi: midi, maskLo: maskLo, maskHi: maskHi)
            let rowY = CGFloat(i) * semitoneHeight
            let h = max(semitoneHeight * blackHeightFraction, 1)
            let y = rowY + (semitoneHeight - h) / 2
            let rect = CGRect(x: 0, y: y, width: blackKeyWidth, height: h)
            let path = Path(roundedRect: rect, cornerRadius: cornerRadius, style: .continuous)
            context.fill(path, with: .color(active ? blackActiveFill : blackFill))
        }
    }
}

// MARK: - Automation Static Layer (extracted subview)
//
// Renders everything that only changes when the user edits the curve or
// pans/zooms: grid, curves, breakpoints, loop region. By splitting this
// out of the main canvas, the 30 Hz display timer no longer causes the
// whole curve to be re-evaluated 900+ times per frame — the layer's
// `Equatable` conformance lets SwiftUI skip the whole Canvas when none
// of its inputs changed.
private struct AutomationStaticLayer: View, Equatable {
    let breakpoints: [AutomationState.UIBreakpoint]
    let selectedIndex: Int?
    let snapEnabled: Bool
    let beatSnapDivision: Double
    let loopEnabled: Bool
    let loopBeats: Double
    let geometry: CurveGeometry

    var body: some View {
        Canvas(rendersAsynchronously: false) { context, size in
            drawGrid(context: &context, size: size)
            drawLoopRegion(context: &context, size: size)
            drawCurves(context: &context, size: size)
            drawBreakpoints(context: &context, size: size)
        }
    }

    private func drawGrid(context: inout GraphicsContext, size: CGSize) {
        let noteCount = geometry.noteRangeHigh - geometry.noteRangeLow
        let noteHeight = size.height / CGFloat(noteCount)

        // Horizontal note lines: batch into two paths (C lines vs. others)
        // so we only stroke twice instead of `noteCount` times.
        var cPath = Path()
        var otherPath = Path()
        for i in 0...noteCount {
            let y = CGFloat(i) * noteHeight
            let midi = geometry.noteRangeHigh - i
            if midi % 12 == 0 {
                cPath.move(to: CGPoint(x: 0, y: y))
                cPath.addLine(to: CGPoint(x: size.width, y: y))
            } else {
                otherPath.move(to: CGPoint(x: 0, y: y))
                otherPath.addLine(to: CGPoint(x: size.width, y: y))
            }
        }
        context.stroke(otherPath, with: .color(.white.opacity(0.05)), lineWidth: 0.25)
        context.stroke(cPath, with: .color(.white.opacity(0.15)), lineWidth: 0.5)

        // Vertical beat grid: batch into three buckets (bar / beat / tick)
        // so we stroke three paths total regardless of how many ticks.
        let tickInterval: Double
        if geometry.viewBeats <= 8 { tickInterval = 0.25 }
        else if geometry.viewBeats <= 16 { tickInterval = 0.5 }
        else if geometry.viewBeats <= 32 { tickInterval = 1.0 }
        else { tickInterval = 2.0 }

        var barPath = Path()
        var beatPath = Path()
        var tickPath = Path()
        let firstTick = (geometry.viewStartBeat / tickInterval).rounded(.up) * tickInterval
        var tick = firstTick
        while tick <= geometry.viewStartBeat + geometry.viewBeats {
            let x = geometry.beatToX(tick, width: size.width)
            let isBar = tick.truncatingRemainder(dividingBy: 4) == 0
            let isBeat = tick.truncatingRemainder(dividingBy: 1) == 0
            if isBar {
                barPath.move(to: CGPoint(x: x, y: 0))
                barPath.addLine(to: CGPoint(x: x, y: size.height))
            } else if isBeat {
                beatPath.move(to: CGPoint(x: x, y: 0))
                beatPath.addLine(to: CGPoint(x: x, y: size.height))
            } else {
                tickPath.move(to: CGPoint(x: x, y: 0))
                tickPath.addLine(to: CGPoint(x: x, y: size.height))
            }
            tick += tickInterval
        }
        context.stroke(tickPath, with: .color(.white.opacity(0.04)), lineWidth: 0.25)
        context.stroke(beatPath, with: .color(.white.opacity(0.08)), lineWidth: 0.25)
        context.stroke(barPath, with: .color(.white.opacity(0.20)), lineWidth: 0.5)

        // Beat snap grid overlay (optional, already rare)
        if snapEnabled && beatSnapDivision > 0 && beatSnapDivision < tickInterval {
            var snapPath = Path()
            let firstSnap = (geometry.viewStartBeat / beatSnapDivision).rounded(.up) * beatSnapDivision
            var snapBeat = firstSnap
            while snapBeat <= geometry.viewStartBeat + geometry.viewBeats {
                let x = geometry.beatToX(snapBeat, width: size.width)
                snapPath.move(to: CGPoint(x: x, y: 0))
                snapPath.addLine(to: CGPoint(x: x, y: size.height))
                snapBeat += beatSnapDivision
            }
            context.stroke(snapPath, with: .color(Color.cyan.opacity(0.06)), lineWidth: 0.25)
        }
    }

    private func drawCurves(context: inout GraphicsContext, size: CGSize) {
        let bps = breakpoints
        guard bps.count >= 2 else { return }

        // Walk the breakpoint list directly instead of sampling once per
        // pixel. Linear segments become a single line; smooth segments are
        // subdivided into a small fixed number of steps; step-hold segments
        // draw a horizontal-then-vertical pair. This is roughly O(bps)
        // instead of O(width) and produces cleaner geometry too.
        var path = Path()
        let startX = geometry.beatToX(bps[0].beat, width: size.width)
        let startY = geometry.semitonesToY(bps[0].semitones, height: size.height)
        path.move(to: CGPoint(x: startX, y: startY))

        for i in 1..<bps.count {
            let prev = bps[i - 1]
            let bp = bps[i]
            let x = geometry.beatToX(bp.beat, width: size.width)
            let y = geometry.semitonesToY(bp.semitones, height: size.height)

            switch prev.interpType {
            case 2: // step
                let prevY = geometry.semitonesToY(prev.semitones, height: size.height)
                path.addLine(to: CGPoint(x: x, y: prevY))
                path.addLine(to: CGPoint(x: x, y: y))
            case 1: // smooth (smoothstep)
                let segments = 16
                let prevX = geometry.beatToX(prev.beat, width: size.width)
                let prevY = geometry.semitonesToY(prev.semitones, height: size.height)
                for s in 1...segments {
                    let t = Double(s) / Double(segments)
                    let sShaped = t * t * (3.0 - 2.0 * t)
                    let xi = prevX + (x - prevX) * CGFloat(sShaped)
                    let yi = prevY + (y - prevY) * CGFloat(sShaped)
                    path.addLine(to: CGPoint(x: xi, y: yi))
                }
            default: // linear
                path.addLine(to: CGPoint(x: x, y: y))
            }
        }

        context.stroke(path, with: .color(Color.cyan.opacity(0.8)), lineWidth: 1.5)
    }

    private func drawBreakpoints(context: inout GraphicsContext, size: CGSize) {
        for (i, bp) in breakpoints.enumerated() {
            let x = geometry.beatToX(bp.beat, width: size.width)
            let y = geometry.semitonesToY(bp.semitones, height: size.height)
            let isSelected = selectedIndex == i

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

    private func drawLoopRegion(context: inout GraphicsContext, size: CGSize) {
        guard loopEnabled && loopBeats > 0 else { return }
        let xEnd = geometry.beatToX(loopBeats, width: size.width)

        if xEnd > 0 && xEnd < size.width {
            context.stroke(
                Path { p in
                    p.move(to: CGPoint(x: xEnd, y: 0))
                    p.addLine(to: CGPoint(x: xEnd, y: size.height))
                },
                with: .color(Color.green.opacity(0.5)),
                style: StrokeStyle(lineWidth: 2, dash: [8, 4])
            )

            let text = Text("\(Int(loopBeats))").font(.system(size: 9, weight: .bold, design: .monospaced))
            context.draw(text, at: CGPoint(x: xEnd - 12, y: 12))
        }

        if xEnd < size.width {
            context.fill(
                Path(CGRect(x: xEnd, y: 0, width: size.width - xEnd, height: size.height)),
                with: .color(Color.black.opacity(0.3))
            )
        }
    }
}

// MARK: - Automation Playhead Layer (extracted subview)
//
// The only piece that genuinely changes every frame. Split into its own
// cheap Canvas so the heavy static layer stays cached.
private struct AutomationPlayheadLayer: View, Equatable {
    let playheadBeat: Double
    let currentPitch: Double
    let autoGlideEnabled: Bool
    let autoGlideTarget: Double
    let geometry: CurveGeometry

    var body: some View {
        Canvas(rendersAsynchronously: false) { context, size in
            drawAutoGlideTarget(context: &context, size: size)
            drawPlayhead(context: &context, size: size)
        }
        .allowsHitTesting(false)
    }

    private func drawPlayhead(context: inout GraphicsContext, size: CGSize) {
        let beat = playheadBeat
        if beat < geometry.viewStartBeat || beat > geometry.viewStartBeat + geometry.viewBeats { return }
        let x = geometry.beatToX(beat, width: size.width)

        context.stroke(
            Path { p in
                p.move(to: CGPoint(x: x, y: 0))
                p.addLine(to: CGPoint(x: x, y: size.height))
            },
            with: .color(.white.opacity(0.6)),
            lineWidth: 1.0
        )

        if abs(currentPitch) > 0.01 {
            let pitchY = geometry.semitonesToY(currentPitch, height: size.height)
            context.fill(
                Path(ellipseIn: CGRect(x: x - 4, y: pitchY - 4, width: 8, height: 8)),
                with: .color(.white)
            )
        }
    }

    private func drawAutoGlideTarget(context: inout GraphicsContext, size: CGSize) {
        guard autoGlideEnabled else { return }
        let y = geometry.semitonesToY(autoGlideTarget, height: size.height)

        context.stroke(
            Path { p in
                p.move(to: CGPoint(x: 0, y: y))
                p.addLine(to: CGPoint(x: size.width, y: y))
            },
            with: .color(Color.orange.opacity(0.5)),
            style: StrokeStyle(lineWidth: 1.5, dash: [6, 4])
        )

        let noteNum = Int(autoGlideTarget.rounded()) + 60
        let pitchClass = ((noteNum % 12) + 12) % 12
        let noteLabel = noteNames[pitchClass]
        let octave = (noteNum / 12) - 1
        let label = "\(noteLabel)\(octave)"
        let text = Text(label).font(.system(size: 10, weight: .bold, design: .monospaced))
        context.draw(text, at: CGPoint(x: size.width - 30, y: y - 10))
    }
}

// MARK: - Automation State

/// Bridges the C++ AutomationCurve to SwiftUI via Obj-C++ bridge methods.
/// Includes undo/redo, beat snapping, and preset restore support.
class AutomationState: ObservableObject {
    struct UIBreakpoint: Identifiable, Equatable {
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

    /// Inserts a new breakpoint at the given position and returns its stable id so
    /// the caller can continue to track it across subsequent sort operations.
    /// Unlike `addBreakpoint`, this does NOT call `pushUndo()` itself — the caller
    /// is expected to wrap the drag in `beginDrag()` / `endDrag()`.
    @discardableResult
    func insertBreakpointReturningID(beat: Double, semitones: Double) -> UUID {
        let snappedBeat = snapBeat(beat)
        let snappedSemi = snapEnabled ? semitones.rounded() : semitones
        let newBP = UIBreakpoint(beat: snappedBeat, semitones: snappedSemi, interpType: interpolationMode)
        breakpoints.append(newBP)
        breakpoints.sort { $0.beat < $1.beat }
        commitToDSP()
        return newBP.id
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
    // Keep these defaults in sync with the AUParameter defaults assigned
    // in `GlideParameters.createParameters()`. Mismatches cause the UI
    // knobs to show stale Swift defaults until the parameter tree
    // propagates its real values, which is visible to the user as
    // "knobs jumping to random values when I touch them".
    @Published var glideTime: Double = 100.0
    @Published var mix: Double = 100.0
    @Published var pitchRange: Double = 12.0
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

        resyncFromTree()
    }

    /// Re-reads every parameter's current value from the tree and pushes
    /// it into the matching `@Published` property. Used to paper over
    /// races between view creation, host state restoration, and preset
    /// loads — any time the tree might contain a value the UI hasn't
    /// seen, call this and the knobs catch up.
    func resyncFromTree() {
        guard let tree = parameterTree else { return }
        for param in tree.allParameters {
            update(address: param.address, value: Double(param.value))
        }
    }

    private func update(address: AUParameterAddress, value: Double) {
        isExternalUpdate = true
        switch address {
        case 0: if glideTime    != value { glideTime    = value }
        case 1: if mix          != value { mix          = value }
        case 2: if pitchRange   != value { pitchRange   = value }
        case 3: if pitchOffset  != value { pitchOffset  = value }
        case 4: if shifterMode  != value { shifterMode  = value }
        case 5: if autoGlide    != value { autoGlide    = value }
        case 6: if loopEnabled  != value { loopEnabled  = value }
        case 7: if loopBeats    != value { loopBeats    = value }
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
    @State private var noteDisplayLatch = NoteDisplayLatch()

    // View range (zoom/scroll)
    @State private var viewBeats: Double = 16.0
    @State private var viewStartBeat: Double = 0.0
    @State private var baseViewBeats: Double = 16.0    // for pinch gesture
    @State private var basePanStart: Double = 0.0      // for pan gesture
    @State private var noteRangeLow: Int = 48
    @State private var noteRangeHigh: Int = 72

    // Drag state for the "grab-the-line-to-bend-it" interaction:
    // tracks which breakpoint (by stable id) is currently being dragged,
    // whether the gesture has actually moved far enough to count as a
    // drag (vs. a tap), and the starting curve semitone value at the
    // click's beat so we can insert a joint on the line.
    @State private var draggingBreakpointID: UUID? = nil
    @State private var dragGestureMoved: Bool = false

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
            audioUnit.onStateRestored = { [weak automation, weak params] in
                // Preset / fullState restore completed. Pull the latest
                // parameter tree values into the UI so the knobs match
                // the DSP (otherwise the UI can be stuck on the Swift
                // defaults while the DSP already has the saved values —
                // which is what the user sees as "the sound is right but
                // the knobs are wrong until I touch one").
                DispatchQueue.main.async {
                    params?.resyncFromTree()
                    automation?.restoreFromDSP()
                }
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

            // Shifter mode (Varispeed / Vocoder)
            Picker("", selection: Binding(
                get: { Int(params.shifterMode) },
                set: { params.sendIfLocal(4, value: Double($0)) }
            )) {
                Text("Varispeed").tag(0)
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
    //
    // Thin wrapper around the extracted `PianoSidebarView` so the
    // 30 Hz display timer can update playhead / levels without
    // rebuilding the piano Canvas. `PianoSidebarView` is Equatable,
    // so SwiftUI skips re-rendering it whenever the note mask hasn't
    // changed.

    private var pianoRollSidebar: some View {
        PianoSidebarView(
            maskLo: activeNoteMask.0,
            maskHi: activeNoteMask.1,
            noteRangeLow: noteRangeLow,
            noteRangeHigh: noteRangeHigh
        )
        .equatable()
    }

    // MARK: - Automation Canvas
    //
    // The automation canvas is split into three layers:
    //   1. `AutomationStaticLayer`  — grid + curves + breakpoints +
    //      loop region. Equatable; only redraws when breakpoints or
    //      view range change.
    //   2. `AutomationPlayheadLayer` — the only piece that moves on
    //      every display-timer tick; cheap Canvas with 1-2 primitives.
    //   3. Gesture layer (Color.clear) — hosts click/drag/pinch.
    // SwiftUI's diffing short-circuits each layer independently, so
    // the playhead updating at 24 Hz no longer re-runs the expensive
    // grid/curve drawing every frame.

    private var automationCanvas: some View {
        GeometryReader { geo in
            let size = geo.size
            let noteCount = noteRangeHigh - noteRangeLow
            let pitchRange = Double(noteCount)
            let geometry = CurveGeometry(
                viewStartBeat: viewStartBeat,
                viewBeats: viewBeats,
                noteRangeLow: noteRangeLow,
                noteRangeHigh: noteRangeHigh
            )

            ZStack {
                AutomationStaticLayer(
                    breakpoints: automation.breakpoints,
                    selectedIndex: automation.selectedIndex,
                    snapEnabled: automation.snapEnabled,
                    beatSnapDivision: automation.beatSnapDivision,
                    loopEnabled: params.loopEnabled > 0.5,
                    loopBeats: params.loopBeats,
                    geometry: geometry
                )
                .equatable()

                AutomationPlayheadLayer(
                    playheadBeat: playheadBeat,
                    currentPitch: currentPitch,
                    autoGlideEnabled: params.autoGlide > 0.5,
                    autoGlideTarget: autoGlideTarget,
                    geometry: geometry
                )
                .equatable()

                // Interaction layer: tap to select, drag to grab-and-bend the curve.
                // Disabled in Auto mode (MIDI drives pitch, not breakpoints).
                //
                // Logic-style interaction (all hit tests are generous — you
                // don't need to be exactly on a breakpoint for click/drag to
                // pick it up):
                //   • Drag on an existing joint → move that joint.
                //   • Drag on empty space → insert a new joint at the drag
                //     start and keep dragging it, so the user grabs the curve
                //     and bends it into a new shape.
                //   • Pure tap on an existing joint → select it.
                //   • Pure tap on empty space → add a joint there.
                //   • Double-click near a joint → delete it.
                //   • Draw mode overrides all of this with the pencil behaviour.
                //
                // A single Color.clear hosts every gesture via simultaneous-
                // Gesture composition so none of them starve each other out.
                let pickThreshold: CGFloat = 22

                Color.clear
                    .contentShape(Rectangle())
                    .allowsHitTesting(params.autoGlide < 0.5)
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onChanged { value in
                                let movement = hypot(value.translation.width, value.translation.height)

                                // Treat sub-3pt wiggles as still-a-tap to keep
                                // click-to-add-joint feeling responsive.
                                if !dragGestureMoved && movement < 3 {
                                    return
                                }

                                if !dragGestureMoved {
                                    dragGestureMoved = true
                                    automation.beginDrag()

                                    if automation.drawMode {
                                        // Draw mode: handled in the movement branch below.
                                    } else if let idx = nearestBreakpointIndex(
                                        at: value.startLocation,
                                        size: size,
                                        pitchRange: pitchRange,
                                        threshold: pickThreshold
                                    ) {
                                        draggingBreakpointID = automation.breakpoints[idx].id
                                    } else {
                                        // No joint under the cursor: insert one at the
                                        // start location, then keep dragging it.
                                        let startBeat = xToBeat(value.startLocation.x, width: size.width)
                                        let startSemi = yToSemitones(value.startLocation.y, height: size.height, range: pitchRange)
                                        draggingBreakpointID = automation.insertBreakpointReturningID(
                                            beat: startBeat,
                                            semitones: startSemi
                                        )
                                    }
                                }

                                let beat = xToBeat(value.location.x, width: size.width)
                                let semi = yToSemitones(value.location.y, height: size.height, range: pitchRange)

                                if automation.drawMode {
                                    automation.drawBreakpoint(beat: beat, semitones: semi)
                                } else if let id = draggingBreakpointID,
                                          let idx = automation.breakpoints.firstIndex(where: { $0.id == id }) {
                                    automation.moveBreakpoint(index: idx, beat: beat, semitones: semi)
                                }
                            }
                            .onEnded { value in
                                if !dragGestureMoved {
                                    // Pure tap: select if near a joint, otherwise add one.
                                    let beat = xToBeat(value.location.x, width: size.width)
                                    let semi = yToSemitones(value.location.y, height: size.height, range: pitchRange)

                                    if let idx = nearestBreakpointIndex(
                                        at: value.location,
                                        size: size,
                                        pitchRange: pitchRange,
                                        threshold: pickThreshold
                                    ) {
                                        automation.selectedIndex = idx
                                    } else if !automation.drawMode {
                                        automation.addBreakpoint(beat: beat, semitones: semi)
                                    }
                                }

                                automation.endDrag()
                                draggingBreakpointID = nil
                                dragGestureMoved = false
                            }
                    )
                    .simultaneousGesture(
                        // Double-click near a joint to delete it. Runs on the
                        // same layer as the drag gesture via simultaneous-
                        // Gesture so it doesn't starve single clicks.
                        SpatialTapGesture(count: 2)
                            .onEnded { event in
                                if let idx = nearestBreakpointIndex(
                                    at: event.location,
                                    size: size,
                                    pitchRange: pitchRange,
                                    threshold: pickThreshold
                                ) {
                                    automation.removeBreakpoint(index: idx)
                                }
                            }
                    )
                    .simultaneousGesture(
                        MagnificationGesture()
                            .onChanged { value in
                                let newBeats = baseViewBeats / Double(value)
                                viewBeats = max(4.0, min(64.0, newBeats))
                            }
                            .onEnded { _ in
                                baseViewBeats = viewBeats
                            }
                    )
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
        // 60 Hz is fast enough to catch even fairly brief MIDI notes
        // (>17 ms) and lets the piano sidebar track rapid sequences in
        // near-real-time. Each @State write is gated by an equality
        // check and the piano sidebar / automation canvas are
        // `Equatable` subviews, so SwiftUI only re-renders the pieces
        // that actually changed — the extra timer wakes cost little.
        displayTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { _ in
            let kernel = audioUnit.kernel

            // Note bitmask is passed through the display latch so brief
            // notes stay visible for a short hold window even after the
            // DSP's real bitmask has cleared.
            let rawLo = kernel.activeNoteBitmaskLo()
            let rawHi = kernel.activeNoteBitmaskHi()
            let display = noteDisplayLatch.update(
                rawMaskLo: rawLo,
                rawMaskHi: rawHi,
                now: CFAbsoluteTimeGetCurrent()
            )
            if display != activeNoteMask { activeNoteMask = display }

            let newBeat = kernel.currentBeatPosition()
            if newBeat != playheadBeat { playheadBeat = newBeat }

            let newPitch = kernel.currentPitchSemitones()
            if newPitch != currentPitch { currentPitch = newPitch }

            let newIn = (kernel.inputLevelL(), kernel.inputLevelR())
            if newIn != inputLevel { inputLevel = newIn }

            let newOut = (kernel.outputLevelL(), kernel.outputLevelR())
            if newOut != outputLevel { outputLevel = newOut }

            let newTarget = kernel.autoGlideTarget()
            if newTarget != autoGlideTarget { autoGlideTarget = newTarget }
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
