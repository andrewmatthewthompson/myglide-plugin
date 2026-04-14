import SwiftUI

struct ContentView: View {
    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "metronome.fill")
                .font(.system(size: 48))
                .foregroundColor(.accentColor)

            Text("MyRythem")
                .font(.largeTitle)
                .fontWeight(.bold)

            Text("Chord Rhythm MIDI Effect")
                .font(.title3)
                .foregroundColor(.secondary)

            Divider().padding(.horizontal, 40)

            Text("This app hosts the MyRythem Audio Unit.\nOpen Logic Pro and add it as a MIDI FX:")
                .multilineTextAlignment(.center)
                .foregroundColor(.secondary)

            Text("MIDI FX \u{2192} Audio Units \u{2192} Demo \u{2192} MyRythem")
                .font(.system(.body, design: .monospaced))
                .padding(8)
                .background(Color.gray.opacity(0.15))
                .cornerRadius(6)
        }
        .padding(40)
        .frame(minWidth: 420, minHeight: 320)
    }
}
