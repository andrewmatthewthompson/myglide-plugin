import SwiftUI

struct ContentView: View {
    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "waveform.path")
                .font(.system(size: 48))
                .foregroundColor(.blue)

            Text("MyGlide")
                .font(.largeTitle)
                .fontWeight(.bold)

            Text("Glide / Portamento Effect")
                .font(.title3)
                .foregroundColor(.secondary)

            Divider().padding(.horizontal, 40)

            Text("This app hosts the MyGlide Audio Unit.\nOpen Logic Pro and add it via:")
                .multilineTextAlignment(.center)
                .foregroundColor(.secondary)

            Text("Audio FX \u{2192} Audio Units \u{2192} Demo \u{2192} MyGlide")
                .font(.system(.body, design: .monospaced))
                .padding(8)
                .background(Color.gray.opacity(0.15))
                .cornerRadius(6)
        }
        .padding(40)
        .frame(minWidth: 400, minHeight: 300)
    }
}
