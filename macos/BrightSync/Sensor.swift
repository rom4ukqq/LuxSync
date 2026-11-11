import Foundation
import Darwin

protocol LightSensor {
    func read() -> Double?
}

struct MockSensor: LightSensor {
    let value: Double
    func read() -> Double? { value }
}

func validatedLux(_ value: Double) -> Double? {
    (0...250_000).contains(value) && value.isFinite ? value : nil
}

// IOKit SPI. Ownership follows the Core Foundation Create/Copy rule.
final class HIDLightSensor: LightSensor {
    private typealias Create = @convention(c) (CFAllocator?) -> Unmanaged<CFTypeRef>?
    private typealias Match = @convention(c) (CFTypeRef, CFDictionary) -> Void
    private typealias Services = @convention(c) (CFTypeRef) -> Unmanaged<CFArray>?
    private typealias Event = @convention(c) (CFTypeRef, Int64, Int32, Int64) -> Unmanaged<CFTypeRef>?
    private typealias Level = @convention(c) (CFTypeRef, Int32) -> Double

    private let library = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL)
    deinit { if let library { dlclose(library) } }

    private func load<T>(_ name: String, _: T.Type) -> T? {
        guard let library, let address = dlsym(library, name) else { return nil }
        return unsafeBitCast(address, to: T.self)
    }

    func read() -> Double? {
        guard let create = load("IOHIDEventSystemClientCreate", Create.self),
              let match = load("IOHIDEventSystemClientSetMatching", Match.self),
              let services = load("IOHIDEventSystemClientCopyServices", Services.self),
              let event = load("IOHIDServiceClientCopyEvent", Event.self),
              let level = load("IOHIDEventGetFloatValue", Level.self),
              let client = create(nil)?.takeRetainedValue() else { return nil }
        match(client, ["PrimaryUsagePage": 0xff00, "PrimaryUsage": 4] as CFDictionary)
        guard let devices = services(client)?.takeRetainedValue() as? [AnyObject] else { return nil }
        for device in devices {
            if let sample = event(device, 12, 0, 0)?.takeRetainedValue(),
               let lux = validatedLux(level(sample, 12 << 16)) { return lux }
        }
        return nil
    }
}
