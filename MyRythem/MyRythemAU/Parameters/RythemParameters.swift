import AVFoundation

/// Parameter addresses — keep in sync with RythemProcessor.hpp ParamAddress enum.
enum RythemParameters {

    enum Address: AUParameterAddress {
        case pattern        = 0
        case rate           = 1
        case gate           = 2
        case swing          = 3
        case velocityMode   = 4
        case fixedVelocity  = 5
        case accent         = 6
        case octave         = 7
        case latch          = 8
        case syncMode       = 9
        case freeBPM        = 10
        case releaseMode    = 11
        case patternLength  = 12
    }

    static let patternNames: [String] = [
        "Straight 8ths",
        "Straight 16ths",
        "Dotted 8ths",
        "8th + 16th",
        "16th + 8th",
        "Dotted 8th + 16th",
        "8th Triplets",
        "Shuffle 8ths",
        "Quarter Pulse",
        "Custom",
    ]

    static let rateNames: [String] = [
        "1/4",
        "1/8",
        "1/8T",
        "1/16",
        "1/16T",
        "1/32",
    ]

    static let velocityModeNames: [String] = ["Fixed", "From Input", "Accented"]
    static let syncModeNames: [String]     = ["Host", "Free"]
    static let releaseModeNames: [String]  = ["Immediate", "Finish Bar"]

    static func createParameters() -> [AUParameter] {
        func makeParam(_ id: String, _ name: String, _ addr: Address,
                       min: AUValue, max: AUValue, unit: AudioUnitParameterUnit,
                       unitName: String? = nil, defaultValue: AUValue,
                       valueStrings: [String]? = nil) -> AUParameter {
            let p = AUParameterTree.createParameter(
                withIdentifier: id,
                name: name,
                address: addr.rawValue,
                min: min,
                max: max,
                unit: unit,
                unitName: unitName,
                flags: [.flag_IsReadable, .flag_IsWritable],
                valueStrings: valueStrings,
                dependentParameters: nil
            )
            p.value = defaultValue
            return p
        }

        let pattern = makeParam("pattern", "Pattern", .pattern,
                                min: 0, max: AUValue(patternNames.count - 1),
                                unit: .indexed, defaultValue: 3,
                                valueStrings: patternNames)

        let rate = makeParam("rate", "Rate", .rate,
                             min: 0, max: AUValue(rateNames.count - 1),
                             unit: .indexed, defaultValue: 3,
                             valueStrings: rateNames)

        let gate = makeParam("gate", "Gate", .gate,
                             min: 5, max: 100, unit: .percent, defaultValue: 60)

        let swing = makeParam("swing", "Swing", .swing,
                              min: 50, max: 75, unit: .percent, defaultValue: 50)

        let velocityMode = makeParam("velocityMode", "Velocity Mode", .velocityMode,
                                     min: 0, max: AUValue(velocityModeNames.count - 1),
                                     unit: .indexed, defaultValue: 1,
                                     valueStrings: velocityModeNames)

        let fixedVelocity = makeParam("fixedVelocity", "Fixed Velocity", .fixedVelocity,
                                      min: 1, max: 127, unit: .generic, defaultValue: 100)

        let accent = makeParam("accent", "Accent", .accent,
                               min: 0, max: 100, unit: .percent, defaultValue: 30)

        let octave = makeParam("octave", "Octave", .octave,
                               min: -2, max: 2, unit: .generic,
                               unitName: "oct", defaultValue: 0)

        let latch = makeParam("latch", "Latch", .latch,
                              min: 0, max: 1, unit: .boolean, defaultValue: 0)

        let syncMode = makeParam("syncMode", "Sync", .syncMode,
                                 min: 0, max: AUValue(syncModeNames.count - 1),
                                 unit: .indexed, defaultValue: 0,
                                 valueStrings: syncModeNames)

        let freeBPM = makeParam("freeBPM", "Free BPM", .freeBPM,
                                min: 30, max: 300, unit: .BPM, defaultValue: 120)

        let releaseMode = makeParam("releaseMode", "Release", .releaseMode,
                                    min: 0, max: AUValue(releaseModeNames.count - 1),
                                    unit: .indexed, defaultValue: 0,
                                    valueStrings: releaseModeNames)

        let patternLength = makeParam("patternLength", "Pattern Length", .patternLength,
                                      min: 1, max: 16, unit: .generic,
                                      unitName: "steps", defaultValue: 16)

        return [
            pattern, rate, gate, swing,
            velocityMode, fixedVelocity, accent,
            octave, latch, syncMode, freeBPM,
            releaseMode, patternLength,
        ]
    }
}
