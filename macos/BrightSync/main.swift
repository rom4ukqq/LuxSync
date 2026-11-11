import AppKit
import Darwin

let args = CommandLine.arguments
if args.dropFirst().first == "--read-brightness" {
    if let level = BuiltInScreenBrightness().readPercent() { print(level); exit(0) }
    fputs("Built-in display brightness unavailable\n", stderr)
    exit(1)
}
if args.dropFirst().first == "--pair-probe" {
    guard args.count == 3, let value = ProcessInfo.processInfo.environment["BRIGHTSYNC_TEST_PIN"],
          let pin = normalizedPin(value), let client = PairingClient(pin: pin, host: args[2]) else { exit(2) }
    var status: Int32?
    client.start { key in status = key == nil ? 1 : 0 }
    withExtendedLifetime(client) {
        while status == nil {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
    }
    exit(status ?? 1)
}
if args.dropFirst().first == "--probe" {
    // Test one real UDP exchange. Keep the test key out of command-line arguments.
    guard args.count == 4, let rawKey = ProcessInfo.processInfo.environment["BRIGHTSYNC_TEST_KEY"],
          let key = parseKey(rawKey), let value = Double(args[3]), let level = validatedBrightness(value) else { exit(2) }
    let sender = Sender(key: key, host: args[2], retryAfter: 1)
    let clock = ProcessInfo.processInfo
    let end = clock.systemUptime + 5
    var nextPoll = 0.0
    while sender.lastAck == nil, clock.systemUptime < end {
        if clock.systemUptime >= nextPoll {
            sender.poll(level)
            nextPoll = clock.systemUptime + 1.5
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
    }
    if sender.lastAck != nil { print("Acknowledged \(level)%"); exit(0) }
    exit(1)
}
let screen: ScreenBrightnessSource
let simulated = args.dropFirst().first == "--simulate"
if simulated {
    guard args.count == 3, let value = Double(args[2]), let level = validatedBrightness(value) else {
        fputs("Usage: BrightSync --simulate <0–100 percent>\n", stderr); exit(2)
    }
    screen = MockScreenBrightness(percent: level)
} else { screen = BuiltInScreenBrightness() }
let app = NSApplication.shared
let delegate = App(screen: screen, simulated: simulated)
app.setActivationPolicy(.accessory)
app.delegate = delegate
app.run()
