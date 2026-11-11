import AppKit
import Darwin

final class Updates {
    private static let account = "github-update-token"
    private let defaults = UserDefaults.standard
    private var operation: Process?
    private var timer: Timer?
    private var next = max(Date().addingTimeInterval(30),
                           (UserDefaults.standard.object(forKey: "updateLastAttempt") as? Date ?? .distantPast)
                               .addingTimeInterval(6 * 60 * 60))
    var canRestart: () -> Bool = { true }

    var automatic: Bool {
        get { defaults.bool(forKey: "automaticUpdates") }
        set {
            defaults.set(newValue, forKey: "automaticUpdates")
            if newValue { next = Date() }
        }
    }

    var busy: Bool { operation != nil }

    func cancel() {
        timer?.invalidate()
        if let operation, operation.isRunning { operation.terminate() }
        operation = nil
    }

    func start() {
        signal(SIGPIPE, SIG_IGN)
        timer = Timer.scheduledTimer(withTimeInterval: 15, repeats: true) { [weak self] _ in
            guard let self, self.automatic, !self.busy, self.next <= Date(), self.canRestart() else { return }
            self.check(manual: false)
        }
        timer?.tolerance = 3
    }

    func configure() -> Bool {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "GitHub update access"
        alert.informativeText = "Enter a fine-grained GitHub token for rom4ukqq/LuxSync with Contents read access."
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 370, height: 24))
        field.placeholderString = "github_pat_…"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Remove Token")
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            let token = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            let bytes = Array(token.utf8)
            guard (20...256).contains(bytes.count), bytes.allSatisfy({
                $0 == 95 || (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0)
            }) else { notice("Invalid token", "Paste the fine-grained token from GitHub."); return false }
            guard BrightSyncKeychain.save(Data(bytes), account: Self.account) == errSecSuccess else {
                notice("Token could not be saved", "Check Keychain access and try again."); return false
            }
            next = Date()
            return true
        case .alertThirdButtonReturn:
            guard BrightSyncKeychain.forget(Self.account) == errSecSuccess else {
                notice("Token could not be removed", "Check Keychain access and try again."); return false
            }
            automatic = false
            return false
        default:
            return false
        }
    }

    func toggleAutomatic() {
        guard automatic || BrightSyncKeychain.load(Self.account) != nil || configure() else { return }
        automatic.toggle()
    }

    func updateNow() {
        guard !busy else { return }
        guard BrightSyncKeychain.load(Self.account) != nil || configure() else { return }
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Update BrightSync?"
        alert.informativeText = "BrightSync will restart if a verified update is available."
        alert.addButton(withTitle: "Check and Install")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        check(manual: true)
    }

    private func check(manual: Bool) {
        next = Date().addingTimeInterval(6 * 60 * 60)
        guard let token = BrightSyncKeychain.load(Self.account) else {
            if manual { notice("Update access required", "Set your GitHub update token first.") }
            return
        }
        defaults.set(Date(), forKey: "updateLastAttempt")
        let bundle = Bundle.main.bundleURL
        let helper = bundle.appendingPathComponent("Contents/Helpers/BrightSyncUpdater")
        let process = Process()
        let input = Pipe()
        process.executableURL = helper
        process.arguments = ["prepare", bundle.path, String(ProcessInfo.processInfo.processIdentifier)]
        process.standardInput = input
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        process.terminationHandler = { [weak self] process in
            let result = process.terminationReason == .exit ? process.terminationStatus : -1
            DispatchQueue.main.async { self?.finished(result, manual: manual) }
        }
        do {
            try process.run()
            operation = process
            try? input.fileHandleForWriting.write(contentsOf: token)
            try? input.fileHandleForWriting.close()
        } catch {
            if manual { notice("Could not start the updater", "Install BrightSync from the full macOS ZIP and try again.") }
        }
    }

    private func finished(_ result: Int32, manual: Bool) {
        operation = nil
        if result == 10 {
            NSApp.terminate(nil)
        } else if manual {
            switch result {
            case 0: notice("BrightSync is up to date", "You have the latest release.")
            case 3: notice("GitHub denied access", "Check the expiration and repository permissions of your token.")
            default: notice("Update could not be installed", "See ~/Library/Caches/BrightSync/update-error.log for details.")
            }
        }
    }

    private func notice(_ title: String, _ text: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.runModal()
    }
}
