import AppKit
import AVFoundation
import Combine
import SonoDSP

struct AudioDevice: Identifiable, Equatable {
    let id: UInt32
    let uid: String
    let name: String
    let inputChannels: Int
    let outputChannels: Int
    let sampleRate: Double
    let bluetooth: Bool

    init(_ raw: SonoDeviceInfo) {
        var raw = raw
        id = raw.id
        uid = withUnsafeBytes(of: &raw.uid) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
        name = withUnsafeBytes(of: &raw.name) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
        inputChannels = Int(raw.input_channels)
        outputChannels = Int(raw.output_channels)
        sampleRate = raw.sample_rate
        bluetooth = raw.bluetooth
    }

    var displayName: String { name.isEmpty ? "Périphérique audio \(id)" : name }
}

@MainActor
final class AudioModel: ObservableObject {
    enum State { case stopped, requestingPermission, monitoring, broadcasting }
    @Published private(set) var devices: [AudioDevice] = []
    @Published var inputID: UInt32 = 0
    @Published var outputID: UInt32 = 0
    @Published var channel = 0
    @Published var showDetails = false
    @Published var gainDB: Double = -18 { didSet { if let audio { sono_audio_gain(audio, Float(gainDB)) } } }
    @Published var protection = true { didSet { if let audio { sono_audio_protection(audio, protection) } } }
    @Published private(set) var state = State.stopped
    @Published private(set) var message = "Choisis ton micro, puis teste son niveau."
    @Published private(set) var isError = false
    @Published private(set) var metrics = SonoAudioStatus()
    @Published private(set) var defaultOutputID: UInt32 = 0
    @Published private(set) var permissionDenied = false
    @Published private(set) var lastIntervention: String?
    private var audio: OpaquePointer?
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []
    private var permissionToken = UUID()
    private var refreshTick = 0
    private var initializedSelection = false

    var inputs: [AudioDevice] { devices.filter { $0.inputChannels > 0 } }
    var outputs: [AudioDevice] { devices.filter { $0.outputChannels > 0 } }
    var input: AudioDevice? { inputs.first { $0.id == inputID } }
    var output: AudioDevice? { outputs.first { $0.id == (outputID == 0 ? defaultOutputID : outputID) } }
    var active: Bool { state == .monitoring || state == .broadcasting }
    var busy: Bool { state == .requestingPermission }
    var broadcasting: Bool { state == .broadcasting }
    var canStart: Bool { input != nil && output != nil && !busy }
    var bluetooth: Bool { output?.bluetooth == true }
    var motionBoom: Bool { output?.name.localizedCaseInsensitiveContains("motion boom") == true }

    init() {
        refreshDevices()
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        let center = NSWorkspace.shared.notificationCenter
        observers.append(center.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.stop("Mac mis en veille. Relance la diffusion à ton retour.") }
        })
        observers.append(center.addObserver(forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.stop("Écran verrouillé ou éteint : diffusion arrêtée.") }
        })
    }

    func refreshDevices() {
        var raw = [SonoDeviceInfo](repeating: SonoDeviceInfo(), count: 128)
        let count = sono_devices(&raw, UInt32(raw.count))
        guard count >= 0 else {
            if active { stop("Impossible de lire les périphériques audio.", error: true) }
            else { message = "Impossible de lire les périphériques audio. Clique sur Actualiser."; isError = true }
            return
        }
        let updated = raw.prefix(Int(count)).map(AudioDevice.init)
            .sorted { $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending }
        if active && (!updated.contains(where: { $0.id == inputID }) ||
                       !updated.contains(where: { $0.id == (outputID == 0 ? defaultOutputID : outputID) })) {
            stop("Un périphérique a été déconnecté. Vérifie les branchements avant de relancer.", error: true)
        }
        devices = updated
        let newDefault = sono_default_output()
        if active && outputID == 0 && newDefault != defaultOutputID {
            stop("La sortie système a changé. Vérifie son volume, puis relance la diffusion.")
        }
        defaultOutputID = newDefault
        if !initializedSelection && !active {
            let savedUID = UserDefaults.standard.string(forKey: "inputUID")
            inputID = inputs.first(where: { $0.uid == savedUID })?.id
                ?? inputs.first(where: { $0.name.localizedCaseInsensitiveContains("fast track") })?.id
                ?? inputs.first(where: { $0.id == sono_default_input() })?.id
                ?? inputs.first?.id ?? 0
            if inputID != 0 { initializedSelection = true }
        }
        if !active && inputs.isEmpty && !isError {
            message = "Aucun micro détecté. Branche la Fast Track, puis clique sur Actualiser."
        }
    }

    func selectInput(_ id: UInt32) {
        guard !active && !busy else { return }
        inputID = id; channel = 0
        if let input { UserDefaults.standard.set(input.uid, forKey: "inputUID") }
    }

    func start(monitorOnly: Bool) {
        guard canStart else { return }
        if let audio, state == .monitoring && !monitorOnly {
            // Every transition to audible output begins at the conservative gain.
            gainDB = -18
            sono_audio_mute(audio, false)
            state = .broadcasting; message = "Diffusion en cours. Monte le gain progressivement."; isError = false
            return
        }
        guard !active else { return }
        isError = false; permissionDenied = false
        state = .requestingPermission; message = "Vérification de l’accès au micro…"
        let token = UUID(); permissionToken = token
        Task {
            try? await Task.sleep(for: .seconds(30))
            guard permissionToken == token, state == .requestingPermission else { return }
            permissionDenied = true
            stop("L’autorisation micro est toujours en attente. Vérifie la demande macOS ou les réglages Microphone, puis réessaie.", error: true)
        }
        Task {
            let allowed: Bool
            switch AVCaptureDevice.authorizationStatus(for: .audio) {
            case .authorized: allowed = true
            case .notDetermined: allowed = await AVCaptureDevice.requestAccess(for: .audio)
            default: allowed = false
            }
            guard permissionToken == token else { return }
            guard allowed else {
                permissionDenied = true
                stop("Accès au micro refusé. Autorise FMSono dans Réglages Système → Confidentialité et sécurité → Microphone.", error: true)
                return
            }
            refreshDevices()
            guard let input, let output, channel < input.inputChannels else {
                stop("Le micro ou la sortie n’est plus disponible. Actualise les périphériques.", error: true)
                return
            }
            gainDB = -18; lastIntervention = nil; metrics = SonoAudioStatus()
            var errorText = [CChar](repeating: 0, count: 512)
            let opened = sono_audio_open(input.id, output.id, UInt32(channel), outputID == 0, &errorText, errorText.count)
            guard let opened else {
                stop(String(cString: errorText), error: true)
                return
            }
            audio = opened
            sono_audio_gain(opened, Float(gainDB))
            sono_audio_protection(opened, protection)
            sono_audio_mute(opened, monitorOnly)
            state = monitorOnly ? .monitoring : .broadcasting
            message = monitorOnly ? "Test du micro : niveau visible, sortie silencieuse." : "Diffusion en cours. Monte le gain progressivement."
        }
    }

    func stop(_ reason: String = "Diffusion arrêtée. Le prochain démarrage se fera à −18 dB.", error: Bool = false) {
        permissionToken = UUID()
        if let audio {
            sono_audio_mute(audio, true)
            metrics = sono_audio_status(audio)
            sono_audio_close(audio)
            self.audio = nil
        }
        metrics.dsp.input_peak = 0; metrics.dsp.output_peak = 0
        state = .stopped; gainDB = -18; message = reason; isError = error
    }

    func emergencyStop() { stop("Coupure d’urgence. Vérifie le volume et la position du micro avant de relancer.") }

    func shutdown() {
        stop(); timer?.invalidate(); timer = nil
        observers.forEach { NSWorkspace.shared.notificationCenter.removeObserver($0) }
        observers.removeAll()
    }

    func openMicrophoneSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone") {
            NSWorkspace.shared.open(url)
        }
    }

    private func tick() {
        if let audio {
            metrics = sono_audio_status(audio)
            if metrics.route_changed {
                stop("La configuration audio a changé. Vérifie la sortie et son volume, puis relance.")
            } else if metrics.error_code != 0 {
                stop("Le flux audio a été interrompu (Core Audio \(metrics.error_code)). Vérifie les connexions.", error: true)
            } else if metrics.dsp.tripped {
                let reason: String
                switch metrics.dsp.fault_reason {
                case 1: reason = "Résonances persistantes : la protection a coupé la diffusion. Éloigne l’enceinte et baisse son volume."
                case 2: reason = "Buffer audio saturé : diffusion coupée pour éviter de restituer un son retardé."
                default: reason = "Flux audio instable : la protection a coupé la diffusion."
                }
                stop(reason, error: true)
            } else if metrics.dsp.notch_count > 0 {
                lastIntervention = String(format: "Résonance suspecte à %.0f Hz • %d filtre(s) • gain réduit de %.0f dB",
                                          metrics.dsp.feedback_hz, metrics.dsp.notch_count, metrics.dsp.safety_reduction_db)
            }
        }
        refreshTick += 1
        if refreshTick >= 15 { refreshTick = 0; refreshDevices() }
    }
}
