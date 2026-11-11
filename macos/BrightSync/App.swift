import AppKit
import ServiceManagement

final class App: NSObject, NSApplicationDelegate, NSMenuDelegate, NSMenuItemValidation {
    private let defaults = UserDefaults.standard
    private let sensor: LightSensor
    private var key: Data?
    private var sender: Sender?
    private var pairingClient: PairingClient?
    private var pairItem: NSMenuItem!
    private var timer: Timer?
    private var lux: Double?
    private var awake = true
    private var item: NSStatusItem!
    private let ambient = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let peer = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private var sync: NSMenuItem!
    private var login: NSMenuItem!
    private let simulated: Bool

    init(sensor: LightSensor, simulated: Bool) { self.sensor = sensor; self.simulated = simulated }
    private var enabled: Bool { defaults.object(forKey: "enabled") as? Bool ?? true }
    private var host: String { defaults.string(forKey: "receiverIP") ?? "" }
    private var interval: Double {
        let value = defaults.object(forKey: "pollSeconds") as? Double ?? 3
        return value.isFinite ? min(8, max(1, value)) : 3
    }

    func applicationDidFinishLaunching(_ note: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.toolTip = "BrightSync"
        item.button?.setAccessibilityLabel("BrightSync Mac Sensor")
        ambient.image = MenuIcon.ambient.image
        peer.image = MenuIcon.receiver.image
        let menu = NSMenu()
        menu.delegate = self
        menu.addItem(NSMenuItem(title: "BrightSync · Mac Sensor", action: nil, keyEquivalent: ""))
        menu.addItem(.separator())
        menu.addItem(ambient)
        menu.addItem(peer)
        menu.addItem(.separator())
        sync = add("Sync Enabled", #selector(toggle), to: menu)
        login = add("Start at Login", #selector(toggleLogin), to: menu)
        menu.addItem(.separator())
        pairItem = add("Pair with Windows…", #selector(pair), to: menu, symbol: .pair)
        _ = add("Forget Pairing…", #selector(forget), to: menu, symbol: .forget)
        _ = add("Windows IP Address…", #selector(address), to: menu, symbol: .address)
        menu.addItem(.separator())
        _ = add("Quit", #selector(quit), to: menu, shortcut: "q")
        item.menu = menu
        let center = NSWorkspace.shared.notificationCenter
        center.addObserver(self, selector: #selector(sleep(_:)), name: NSWorkspace.willSleepNotification, object: nil)
        center.addObserver(self, selector: #selector(wake(_:)), name: NSWorkspace.didWakeNotification, object: nil)
        key = Pairing.load()
        restart()
    }

    private func add(_ title: String, _ action: Selector, to menu: NSMenu,
                     shortcut: String = "", symbol: MenuIcon? = nil) -> NSMenuItem {
        let entry = NSMenuItem(title: title, action: action, keyEquivalent: shortcut)
        entry.target = self
        if let symbol { entry.image = symbol.image }
        menu.addItem(entry)
        return entry
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        if menuItem == pairItem { return pairingClient == nil }
        if menuItem.action == #selector(forget) { return key != nil && pairingClient == nil }
        return true
    }

    func menuWillOpen(_ menu: NSMenu) { refresh() }

    private func refresh() {
        ambient.title = lux.map { String(format: "Ambient: %.1f lux%@", $0, simulated ? " (simulated)" : "") }
            ?? (enabled ? "Ambient: Unavailable" : "Ambient: Paused")
        let connected = sender?.lastAck.map { ProcessInfo.processInfo.systemUptime - $0 < max(10, interval * 3) } ?? false
        let status = pairingClient != nil ? "Pairing…" : !enabled ? "Paused" : key == nil ? "Not paired" : sender == nil ? "Network unavailable" :
            connected ? "Connected" : "Waiting for receiver"
        peer.title = "Windows PC: " + status
        item.button?.image = (connected && enabled ? MenuIcon.connected : MenuIcon.sensor).image
        item.button?.toolTip = "BrightSync · " + status
        sync.state = enabled ? .on : .off
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    private func restart() {
        timer?.invalidate(); timer = nil
        sender = nil; lux = nil
        guard awake, enabled else { refresh(); return }
        tick()
        let repeating = Timer(timeInterval: interval, repeats: true) { [weak self] _ in self?.tick() }
        repeating.tolerance = 0.2
        RunLoop.main.add(repeating, forMode: .common)
        timer = repeating
    }

    private func tick() {
        if sender == nil, let key {
            sender = Sender(key: key, host: host)
            sender?.onAck = { [weak self] in self?.refresh() }
        }
        lux = sensor.read()
        if let lux { sender?.poll(lux) }
        refresh()
    }

    @objc private func sleep(_ note: Notification) {
        pairingClient?.cancel()
        pairingClient = nil
        awake = false
        restart()
    }
    @objc private func wake(_ note: Notification) { awake = true; restart() }
    @objc private func toggle() { defaults.set(!enabled, forKey: "enabled"); restart() }
    @objc private func quit() { NSApp.terminate(nil) }

    @objc private func toggleLogin() {
        let service = SMAppService.mainApp
        do {
            switch service.status {
            case .enabled: try service.unregister()
            case .requiresApproval: SMAppService.openSystemSettingsLoginItems()
            default: try service.register()
            }
        } catch { notice("Could not change login settings", error.localizedDescription) }
        refresh()
    }

    @objc private func pair() {
        guard pairingClient == nil else { return }
        let alert = prompt("Pair with Windows", "In the Windows tray, choose Show Pairing Code. Enter the eight characters here.")
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 26))
        field.placeholderString = "ABCD-2345"
        field.font = .monospacedSystemFont(ofSize: 17, weight: .medium)
        field.setAccessibilityLabel("Eight-character pairing code")
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        guard let pin = normalizedPin(field.stringValue) else {
            notice("Check the pairing code", "Use the eight letters and numbers shown on Windows.")
            return
        }
        guard let client = PairingClient(pin: pin, host: host) else {
            notice("Could not start pairing", "Check the Windows IP address and try again.")
            return
        }
        pairingClient = client
        refresh()
        client.start { [weak self] key in
            guard let self else { return }
            self.pairingClient = nil
            self.refresh()
            guard let key else {
                self.notice("Could not pair with Windows", "Check the code and make sure both computers are on the same network. You can also enter the PC’s address under Windows IP Address.")
                return
            }
            let status = Pairing.save(key)
            guard status == errSecSuccess else {
                self.notice("Could not save pairing", "Keychain error \(status).")
                return
            }
            self.key = key
            self.restart()
            self.notice("Paired with Windows", "BrightSync will now send readings automatically.")
        }
    }

    @objc private func forget() {
        guard prompt("Forget Windows pairing?", "This removes the stored key from this Mac.").runModal()
                == .alertFirstButtonReturn else { return }
        let status = Pairing.forget()
        guard status == errSecSuccess else { notice("Could not remove key", "Keychain error \(status)."); return }
        key = nil
        restart()
    }

    @objc private func address() {
        let alert = prompt("Windows IP address", "Enter the Windows PC's IPv4 address, or leave this blank for automatic discovery.")
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        field.stringValue = host
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let value = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard value.isEmpty || ipv4(value) != nil else { notice("Invalid IP address", "Use an IPv4 address such as 192.168.1.20."); return }
        defaults.set(value, forKey: "receiverIP")
        restart()
    }

    private func prompt(_ title: String, _ detail: String) -> NSAlert {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title; alert.informativeText = detail
        alert.addButton(withTitle: "OK"); alert.addButton(withTitle: "Cancel")
        return alert
    }

    private func notice(_ title: String, _ detail: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title; alert.informativeText = detail
        alert.runModal()
    }
}
