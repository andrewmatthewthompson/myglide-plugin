import AVFoundation

/// Parameter addresses — keep in sync with GlideProcessor.hpp ParamAddress enum
enum GlideParameters {

    enum Address: AUParameterAddress {
        case glideTime  = 0   // Pitch smoothing rate (ms)
        case mix        = 1   // Wet/dry mix (%)
        case pitchRange = 2   // Display range: 12 or 24 semitones
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
        ]
    }
}
