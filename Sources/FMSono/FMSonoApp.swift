import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    var shutdown: (() -> Void)?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { shutdown?() }
}

@main
struct FMSonoApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AudioModel()

    var body: some Scene {
        Window("FMSono", id: "main") {
            ContentView(model: model)
                .onAppear { delegate.shutdown = { model.shutdown() } }
                .frame(minWidth: 580, idealWidth: 700, minHeight: 680, idealHeight: 760)
        }
        .defaultSize(width: 700, height: 760)
        .windowResizability(.contentMinSize)
        .commands {
            CommandMenu("Audio") {
                Button("Couper immédiatement") { model.emergencyStop() }
                    .keyboardShortcut("m", modifiers: [.command, .shift])
                Button("Actualiser les périphériques") { model.refreshDevices() }
                    .keyboardShortcut("r", modifiers: [.command])
                    .disabled(model.active || model.busy)
            }
        }
    }
}
