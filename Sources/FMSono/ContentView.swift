import SwiftUI

struct ContentView: View {
    @ObservedObject var model: AudioModel

    private var statusColor: Color { model.isError ? .red : model.broadcasting ? .teal : .secondary }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    header
                    routing
                    if model.bluetooth { bluetoothNotice }
                    levels
                    amplification
                    protection
                    DisclosureGroup("Détails audio et conseils", isExpanded: $model.showDetails) { details.padding(.top, 12) }
                        .font(.callout)
                    Text("Traitement local · aucun enregistrement · aucun envoi réseau")
                        .font(.caption).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity)
                }
                .padding(28)
            }
            Divider()
            controls.padding(.horizontal, 28).padding(.vertical, 18)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .tint(.teal)
    }

    private var header: some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: "waveform.circle.fill")
                .font(.system(size: 48)).foregroundStyle(.teal)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 5) {
                Text("FMSono").font(.largeTitle.weight(.semibold))
                Text("Ta voix, amplifiée par ton Mac.").foregroundStyle(.secondary)
            }
            Spacer()
            Label(model.broadcasting ? "En direct" : model.active ? "Test micro" : "À l’arrêt",
                  systemImage: model.broadcasting ? "dot.radiowaves.left.and.right" : "speaker.slash")
                .font(.caption.weight(.semibold))
                .padding(.horizontal, 12).padding(.vertical, 8)
                .background(statusColor.opacity(0.12), in: Capsule())
                .foregroundStyle(statusColor)
        }
    }

    private var routing: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label("Périphériques", systemImage: "cable.connector").font(.headline)
                Spacer()
                Button { model.refreshDevices() } label: { Label("Actualiser", systemImage: "arrow.clockwise") }
                    .buttonStyle(.borderless).disabled(model.active || model.busy)
                    .help("Relire les micros et sorties connectés au Mac")
            }
            Picker("Micro", selection: Binding(get: { model.inputID }, set: { model.selectInput($0) })) {
                Text("Choisir un micro").tag(UInt32(0))
                ForEach(model.inputs) { Text($0.displayName).tag($0.id) }
            }
            HStack {
                Picker("Canal", selection: $model.channel) {
                    ForEach(0..<max(1, model.input?.inputChannels ?? 1), id: \.self) { Text("Canal \($0 + 1)").tag($0) }
                }.frame(maxWidth: 230)
                Text("Un seul canal, diffusé sur les deux haut-parleurs.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Picker("Sortie", selection: $model.outputID) {
                Text("Sortie système par défaut").tag(UInt32(0))
                ForEach(model.outputs) { Text($0.displayName).tag($0.id) }
            }
            Label(model.output?.displayName ?? "Aucune sortie disponible", systemImage: model.bluetooth ? "antenna.radiowaves.left.and.right" : "speaker.wave.2")
                .font(.caption).foregroundStyle(.secondary)
        }
        .disabled(model.active || model.busy)
        .padding(18).background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 16))
    }

    private var bluetoothNotice: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "clock.badge.exclamationmark").font(.title2).foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 5) {
                Text(model.motionBoom ? "Motion Boom : délai Bluetooth" : "Sortie Bluetooth : délai possible")
                    .font(.callout.weight(.semibold))
                Text("Ta voix peut revenir avec un retard gênant. FMSono ne peut pas supprimer ce délai. Une enceinte filaire est préférable pour parler ou chanter en direct.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(14).background(Color.orange.opacity(0.10), in: RoundedRectangle(cornerRadius: 12))
    }

    private var levels: some View {
        HStack(spacing: 24) {
            LevelMeter(title: "Entrée micro", peak: model.metrics.dsp.input_peak, icon: "mic.fill", color: .teal)
            LevelMeter(title: "Sortie amplifiée", peak: model.metrics.dsp.output_peak, icon: "speaker.wave.2.fill", color: .blue)
        }
    }

    private var amplification: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("Gain de la voix", systemImage: "slider.horizontal.3").font(.headline)
                Spacer()
                Text(String(format: "%+.0f dB", model.gainDB)).font(.title3.monospacedDigit().weight(.medium))
            }
            Slider(value: $model.gainDB, in: -36...12, step: 1)
                .accessibilityLabel("Gain de la voix")
                .accessibilityValue(String(format: "%.0f décibels", model.gainDB))
                .disabled(!model.broadcasting)
            HStack { Text("Plus doux"); Spacer(); Text("Plus fort") }.font(.caption).foregroundStyle(.secondary)
            Text("Départ à −18 dB. Le volume de l’enceinte et du Mac compte aussi.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var protection: some View {
        VStack(alignment: .leading, spacing: 9) {
            Toggle(isOn: $model.protection) {
                Label("Protection anti-larsen · voix parlée", systemImage: "shield.lefthalf.filled")
                    .font(.headline)
            }.toggleStyle(.switch).disabled(model.active || model.busy)
            Text(model.protection
                 ? "Atténue les résonances suspectes et coupe si elles persistent. Une note tenue peut aussi déclencher la protection."
                 : "Détection des résonances désactivée. Le limiteur et la coupure d’urgence restent actifs.")
                .font(.caption).foregroundStyle(model.protection ? Color.secondary : Color.orange)
                .fixedSize(horizontal: false, vertical: true)
            if let intervention = model.lastIntervention {
                Label(intervention, systemImage: "exclamationmark.triangle.fill")
                    .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
            Text("La protection réduit le risque ; elle ne garantit pas l’absence de larsen.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label(model.message, systemImage: model.isError ? "exclamationmark.triangle.fill" : "info.circle")
                .font(.callout).foregroundStyle(model.isError ? Color.red : Color.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("audioStatus")
            if model.permissionDenied {
                Button("Ouvrir les réglages du micro") { model.openMicrophoneSettings() }
            }
            HStack(spacing: 12) {
                Button { model.start(monitorOnly: false) } label: {
                    Label(model.busy ? "Préparation…" : "Diffuser", systemImage: "play.fill")
                        .frame(minWidth: 100)
                }
                .buttonStyle(.borderedProminent).controlSize(.large)
                .disabled(!model.canStart || model.broadcasting)
                Button { model.active ? model.stop() : model.start(monitorOnly: true) } label: {
                    Label(model.active ? "Arrêter" : "Tester le micro", systemImage: model.active ? "stop.fill" : "mic")
                }.controlSize(.large).disabled(model.busy || (!model.active && !model.canStart))
                Spacer(minLength: 0)
                Button(role: .destructive) { model.emergencyStop() } label: {
                    Label("Couper", systemImage: "speaker.slash.fill")
                }
                .buttonStyle(.bordered).controlSize(.large)
                .keyboardShortcut(.cancelAction)
                .help("Couper immédiatement · Échap ou ⌘⇧M. Le Bluetooth peut encore jouer le son déjà en mémoire.")
            }
            Text("Coupure : Échap ou ⌘⇧M. En Bluetooth, le son déjà envoyé peut encore être audible brièvement.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 10) {
            if model.active {
                LabeledContent("Formats", value: String(format: "%.0f → %.0f Hz", model.metrics.input_rate, model.metrics.output_rate))
                LabeledContent("Délai estimé par les buffers", value: String(format: "≈ %.0f ms", model.metrics.estimated_latency_ms))
                Text("Estimation technique, pas une mesure acoustique. Le délai Bluetooth réel peut être nettement supérieur.")
                    .font(.caption).foregroundStyle(.secondary)
                LabeledContent("Réduction du limiteur", value: String(format: "%.1f dB", model.metrics.dsp.limiter_reduction_db))
                LabeledContent("Coupures de buffer", value: "\(model.metrics.dsp.underruns)")
            }
            Text("Pour le premier essai : règle l’enceinte très bas, garde le micro près de la bouche et éloigne l’enceinte du micro. Sur la Fast Track, sélectionne le canal du micro et ajuste le préampli sans saturation.")
                .fixedSize(horizontal: false, vertical: true)
            Text("Filtre coupe-bas à 90 Hz • limiteur à −6 dBFS • réduction automatique du gain • arrêt après plusieurs résonances. Le limiteur numérique ne fixe pas le volume acoustique de l’enceinte.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if model.motionBoom {
                Text("La Motion Boom originale ne dispose pas d’entrée AUX. Le câble USB sert à la charge, pas à contourner la liaison Bluetooth.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

private struct LevelMeter: View {
    let title: String
    let peak: Float
    let icon: String
    let color: Color
    private var db: Double { max(-60, 20 * log10(max(Double(peak), 0.000001))) }
    private var proportion: Double { min(1, max(0, (db + 60) / 60)) }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label(title, systemImage: icon).font(.callout.weight(.medium))
                Spacer()
                Text(peak < 0.00001 ? "Silence" : String(format: "%.0f dBFS", db))
                    .font(.caption.monospacedDigit()).foregroundStyle(peak >= 0.98 ? Color.red : Color.secondary)
            }
            GeometryReader { geometry in
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.secondary.opacity(0.15))
                    if proportion > 0 {
                        Capsule().fill(peak >= 0.98 ? Color.red : color)
                            .frame(width: max(1, geometry.size.width * proportion))
                    }
                }
            }.frame(height: 8)
            Text(peak >= 0.98 ? "Saturation : baisse le gain de l’interface" : "Niveau crête")
                .font(.caption2).foregroundStyle(peak >= 0.98 ? Color.red : Color.secondary)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(title)
        .accessibilityValue(peak < 0.00001 ? "Silence" : String(format: "%.0f décibels pleine échelle", db))
    }
}
