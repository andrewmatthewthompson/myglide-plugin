import AVFoundation

/// Parameter addresses — keep in sync with GlideProcessor.hpp ParamAddress enum
enum GlideParameters {

    enum Address: AUParameterAddress {
        case glideTime   = 0   // Pitch smoothing rate (ms)
        case mix         = 1   // Wet/dry mix (%)
        case pitchRange  = 2   // Display range: 12 or 24 semitones
        case pitchOffset = 3   // DAW-automatable pitch offset (semitones)
        case shifterMode = 4   // 0=Granular, 1=Vocoder
        case autoGlide   = 5   // 0=Manual, 1=Auto (MIDI-driven)
        case loopEnabled = 6   // 0=off, 1=on
        case loopBeats   = 7   // Loop length in beats
    }

    static func createParameters() -> [AUParameter] {
        return [
            AUParameterTree.createParameter(
                withIdentifier: "glideTime",
                name: "Glide Time",
                address: Address.glideTime.rawValue,
                min: 1,
                max: 2000,
                unit: .milliseconds,
                unitName: nil,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "mix",
                name: "Mix",
                address: Address.mix.rawValue,
                min: 0,
                max: 100,
                unit: .percent,
                unitName: nil,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "pitchRange",
                name: "Pitch Range",
                address: Address.pitchRange.rawValue,
                min: 12,
                max: 24,
                unit: .generic,
                unitName: "semi",
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "pitchOffset",
                name: "Pitch Offset",
                address: Address.pitchOffset.rawValue,
                min: -24,
                max: 24,
                unit: .generic,
                unitName: "semi",
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "shifterMode",
                name: "Shifter Mode",
                address: Address.shifterMode.rawValue,
                min: 0,
                max: 1,
                unit: .indexed,
                unitName: nil,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "autoGlide",
                name: "Auto Glide",
                address: Address.autoGlide.rawValue,
                min: 0,
                max: 1,
                unit: .boolean,
                unitName: nil,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "loopEnabled",
                name: "Loop",
                address: Address.loopEnabled.rawValue,
                min: 0,
                max: 1,
                unit: .boolean,
                unitName: nil,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
            AUParameterTree.createParameter(
                withIdentifier: "loopBeats",
                name: "Loop Length",
                address: Address.loopBeats.rawValue,
                min: 4,
                max: 64,
                unit: .beats,
                unitName: nil,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: nil,
                dependentParameters: nil
            ),
        ]
    }
}
