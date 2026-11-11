import CoreGraphics
import Darwin

protocol ScreenBrightnessSource {
    func readPercent() -> Double?
}

struct MockScreenBrightness: ScreenBrightnessSource {
    let percent: Double
    func readPercent() -> Double? { percent }
}

func validatedBrightness(_ value: Double) -> Double? {
    value.isFinite && (0...100).contains(value) ? value : nil
}

final class BuiltInScreenBrightness: ScreenBrightnessSource {
    private typealias GetBrightness = @convention(c) (CGDirectDisplayID, UnsafeMutablePointer<Float>) -> Int32
    private let library: UnsafeMutableRawPointer?
    private let getBrightness: GetBrightness?

    init() {
        let framework = "/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices"
        library = dlopen(framework, RTLD_LAZY | RTLD_LOCAL)
        // DisplayServices has no public SDK header. Keep the SPI isolated and read-only.
        getBrightness = library.flatMap { dlsym($0, "DisplayServicesGetBrightness") }
            .map { unsafeBitCast($0, to: GetBrightness.self) }
    }

    deinit { if let library { dlclose(library) } }

    func readPercent() -> Double? {
        guard let getBrightness else { return nil }
        var displays = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(UInt32(displays.count), &displays, &count) == .success,
              let panel = displays.prefix(Int(count)).first(where: {
                  CGDisplayIsBuiltin($0) != 0 && CGDisplayIsAsleep($0) == 0
              }) else { return nil }
        var brightness: Float = 0
        guard getBrightness(panel, &brightness) == 0 else { return nil }
        return validatedBrightness(Double(brightness) * 100)
    }
}
