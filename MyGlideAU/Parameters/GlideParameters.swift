import AVFoundation

/// Parameter addresses — keep in sync with GlideDSPKernel.hpp ParamAddress enum
enum GlideParameters {

    enum Address: AUParameterAddress {
        case glideTime  = 0
        case mix        = 1
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
        ]
    }
}
