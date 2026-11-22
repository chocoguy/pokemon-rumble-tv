import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        NavigationSplitView {
            List(selection: $model.screen) {
                Label("Setup", systemImage: "wrench.and.screwdriver")
                    .badge(model.setupIssues)
                    .tag(AppModel.Screen.setup)
                Label("Game", systemImage: "gamecontroller")
                    .tag(AppModel.Screen.game)
                Label("Updates", systemImage: "arrow.down.circle")
                    .badge(model.updateAvailable ? 1 : 0)
                    .tag(AppModel.Screen.updates)
                Label {
                    Text("Activity")
                } icon: {
                    if model.activity.running {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "list.bullet.rectangle")
                    }
                }
                .tag(AppModel.Screen.activity)
            }
            .navigationSplitViewColumnWidth(170)
        } detail: {
            ScrollView {
                Group {
                    switch model.screen {
                    case .setup: SetupScreen()
                    case .game: GameScreen()
                    case .updates: UpdatesScreen()
                    case .activity: ActivityScreen()
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(24)
            }
        }
        .frame(minWidth: 760, minHeight: 520)
    }
}

// MARK: - Setup

struct SetupScreen: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Header(title: "Setup", subtitle: "What this Mac needs to build the game for your Apple TV.")
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    if model.doctor.isEmpty {
                        ProgressView("Checking…")
                    }
                    ForEach(model.doctor) { item in
                        HStack(alignment: .top, spacing: 10) {
                            StatusIcon(status: item.status)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(item.title).bold()
                                Text(item.detail).font(.callout).foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            Spacer()
                            if let action = item.action, let title = item.actionTitle {
                                Button(title) { model.perform(action) }
                                    .disabled(model.activity.running)
                            }
                        }
                    }
                }
                .padding(8)
            }
            HStack {
                Button("Check Again") {
                    Task { await model.checkSetup() }
                }
                .disabled(model.checkingSetup)
                if model.checkingSetup {
                    ProgressView().controlSize(.small)
                }
            }
            GameFilesBox()
        }
    }
}

struct StatusIcon: View {
    let status: DoctorItem.Status

    var body: some View {
        switch status {
        case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .missing: Image(systemName: "xmark.circle.fill").foregroundStyle(.red)
        case .optional: Image(systemName: "minus.circle").foregroundStyle(.secondary)
        case .checking: ProgressView().controlSize(.small)
        }
    }
}

struct GameFilesBox: View {
    @EnvironmentObject private var model: AppModel
    @State private var dropping = false

    var body: some View {
        let found = model.gameContent
        GroupBox {
            HStack(spacing: 12) {
                Image(systemName: found == nil ? "questionmark.folder" : "checkmark.circle.fill").font(.title)
                    .foregroundStyle(found == nil ? Color.secondary : Color.green)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title(found)).font(.headline)
                    Text(found == nil
                         ? "Open your own Pokémon Rumble (USA) WAD in Dolphin with Tools → Install WAD, and the installer finds it here. Or choose Dolphin's user folder, or a folder of the decrypted .app files."
                         : found!.path)
                        .font(.caption).foregroundStyle(.secondary).lineLimit(3).truncationMode(.middle)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                VStack(alignment: .trailing) {
                    Button("Choose…", action: choose).disabled(model.activity.running)
                    if !model.state.game.isEmpty {
                        Button("Use Dolphin's") { model.state.game = "" }
                            .buttonStyle(.link).font(.caption).disabled(model.activity.running)
                    }
                }
            }
            .padding(6)
            .background(RoundedRectangle(cornerRadius: 6).stroke(dropping ? Color.accentColor : .clear, lineWidth: 2))
            .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
                guard !model.activity.running, let provider = providers.first else {
                    return false
                }
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    if let url {
                        Task { @MainActor in model.state.game = url.path }
                    }
                }
                return true
            }
        } label: {
            Text("Your game files").font(.headline)
        }
    }

    private func title(_ found: URL?) -> String {
        if found == nil {
            return model.state.game.isEmpty ? "Not installed in Dolphin yet" : "No game files in that folder"
        }
        return model.state.game.isEmpty ? "Found in Dolphin" : URL(fileURLWithPath: model.state.game).lastPathComponent
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.message = "Choose Dolphin's user folder (it holds Wii), or a folder of Pokémon Rumble's decrypted .app files."
        panel.prompt = "Choose"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.directoryURL = GameFiles.dolphin
        if panel.runModal() == .OK, let url = panel.url {
            model.state.game = url.path
        }
    }
}

// MARK: - Game

struct GameScreen: View {
    @EnvironmentObject private var model: AppModel
    @State private var adding: Platform?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Header(title: "Pokémon Rumble TV",
                   subtitle: model.state.tag.map { "Version \($0) is built." } ?? "Not built yet. The first build takes about 10 minutes.")
            GameFilesBox()
            GroupBox {
                VStack(alignment: .leading, spacing: 10) {
                    if model.state.installs.isEmpty {
                        Text("Not installed anywhere yet.").foregroundStyle(.secondary)
                    }
                    ForEach(model.state.installs) { install in
                        InstallRow(install: install)
                        Divider()
                    }
                    HStack {
                        Button("Install on an Apple TV…") { adding = .tvos }
                            .disabled(!model.canRun)
                        if !model.pending.isEmpty {
                            Button("Finish the Update", action: model.retryPending).disabled(!model.canRun)
                        }
                        Spacer()
                        if let blocker = model.blocker {
                            Text(blocker).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                .padding(8)
            } label: {
                Text("Installed on").font(.headline)
            }
        }
        .sheet(item: $adding) { platform in
            DevicePicker(platform: platform) { device in
                adding = nil
                if let device {
                    model.add(platform, device: device)
                }
            }
        }
    }
}

struct InstallRow: View {
    @EnvironmentObject private var model: AppModel
    let install: Install

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: install.platform.symbol).font(.title2).frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(install.deviceName).bold()
                Text(details).font(.caption).foregroundStyle(install.pending || expiringSoon ? .orange : .secondary)
            }
            Spacer()
            Button("Reinstall") { model.reinstall(install) }.disabled(!model.canRun)
            Menu {
                Button("Remove from List", role: .destructive) { model.forget(install) }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
        }
    }

    private var expiringSoon: Bool {
        install.expires.map { $0.timeIntervalSinceNow < 3 * 86400 } ?? false
    }

    private var details: String {
        var parts: [String] = []
        parts.append("Version \(install.version ?? "unknown")")
        if let now = model.reported[install.id], now != install.version {
            parts.append("reports: \(now)")
        }
        if let expires = install.expires {
            parts.append((expires < .now ? "stopped opening " : "works until ") +
                         expires.formatted(date: .abbreviated, time: .omitted))
        }
        if install.pending {
            parts.append("the last update didn't reach it")
        }
        return parts.joined(separator: " · ")
    }
}

struct DevicePicker: View {
    let platform: Platform
    let done: (Device?) -> Void
    @State private var devices: [Device] = []
    @State private var loading = true
    @State private var chosen: Device?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Install on an \(platform.title)").font(.title2.bold())
            Text("Pair it in Xcode's Devices window (on the Apple TV: Settings → Remotes and Devices → Remote App and Devices), turn on Developer Mode on it, and keep it awake.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Picker("Device", selection: $chosen) {
                    Text(loading ? "Looking…" : devices.isEmpty ? "None found" : "Choose…").tag(Device?.none)
                    ForEach(devices) { device in
                        Text(device.title).tag(Device?.some(device))
                    }
                }
                Button {
                    Task { await load() }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(loading)
            }
            HStack {
                Spacer()
                Button("Cancel") { done(nil) }.keyboardShortcut(.cancelAction)
                Button("Install") { done(chosen) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(chosen == nil)
            }
        }
        .padding(24)
        .frame(width: 460)
        .task { await load() }
    }

    private func load() async {
        loading = true
        devices = await Device.list(platform)
        if devices.count == 1 {
            chosen = devices[0]
        }
        loading = false
    }
}

// MARK: - Updates

struct UpdatesScreen: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var updater: InstallerUpdater

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Header(title: "Updates", subtitle: "New versions come from the project's releases on GitHub.")
            GroupBox {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Rumble TV Installer \(model.installerVersion.description)").bold()
                        if let needed = model.needsInstaller {
                            Text("The latest game needs installer \(needed.description) or later.").foregroundStyle(.orange)
                        }
                    }
                    Spacer()
                    Button("Check for Updates…", action: updater.check).disabled(!updater.available)
                }
                .padding(8)
            }
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    if let error = model.releaseError {
                        Label(error, systemImage: "wifi.exclamationmark").foregroundStyle(.orange)
                    }
                    if let latest = model.latest {
                        HStack(alignment: .firstTextBaseline) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(latest.title).font(.title3.bold())
                                Text(status(latest)).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button(model.state.installs.isEmpty ? "Nothing Installed Yet" : "Update All", action: model.updateAll)
                                .buttonStyle(.borderedProminent)
                                .disabled(!model.updateAvailable || !model.canRun)
                        }
                        if let notes = latest.notes, !notes.isEmpty {
                            Divider()
                            Text((try? AttributedString(markdown: notes, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(notes))
                                .textSelection(.enabled)
                        }
                    } else if model.releaseError == nil {
                        Text("No release has been published yet.").foregroundStyle(.secondary)
                    }
                    HStack {
                        Button("Check Again") {
                            Task { await model.loadReleases() }
                        }
                        Spacer()
                        Menu("Install Another Version…") {
                            ForEach(model.installable) { release in
                                Button(release.tag + (release.tag == model.state.tag ? " (built)" : "")) {
                                    model.install(release)
                                }
                            }
                        }
                        .fixedSize()
                        .disabled(model.installable.isEmpty || model.state.installs.isEmpty || !model.canRun)
                    }
                }
                .padding(8)
            } label: {
                Text("The game").font(.headline)
            }
        }
    }

    private func status(_ latest: Release) -> String {
        guard let tag = model.state.tag else {
            return "The newest release. Install the game from the Game screen."
        }
        return model.updateAvailable ? "You have \(tag). Update All rebuilds it once and reinstalls it everywhere it's installed."
                                     : "You have the newest version."
    }
}

// MARK: - Activity

struct ActivityScreen: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        let activity = model.activity
        VStack(alignment: .leading, spacing: 14) {
            Header(title: activity.title.isEmpty ? "Activity" : activity.title,
                   subtitle: activity.title.isEmpty ? "Nothing has run yet." : "")
            if activity.running {
                VStack(alignment: .leading, spacing: 6) {
                    Text(activity.stage.isEmpty ? "Starting…" : activity.stage)
                    if let progress = activity.progress {
                        ProgressView(value: progress)
                    } else {
                        ProgressView().progressViewStyle(.linear)
                    }
                }
            } else if let problem = activity.problem {
                HStack(alignment: .top) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(problem.message).bold()
                        if !problem.fix.isEmpty {
                            Text(problem.fix).foregroundStyle(.secondary).textSelection(.enabled)
                        }
                    }
                }
            } else if activity.cancelled {
                Text("Cancelled.").foregroundStyle(.secondary)
            } else if let finished = activity.finished {
                Label(finished, systemImage: "checkmark.seal.fill").foregroundStyle(.green)
            }
            ForEach(Array(activity.notes.enumerated()), id: \.offset) { note in
                Label(note.element, systemImage: "info.circle").font(.callout).foregroundStyle(.secondary)
            }
            HStack {
                if activity.running {
                    Button("Cancel", action: model.cancel)
                }
                Spacer()
                Button("Copy Diagnostics", action: model.copyDiagnostics).disabled(activity.log.isEmpty)
                Button("Show Logs") { NSWorkspace.shared.open(model.workspace.logs) }
            }
            GroupBox {
                ScrollViewReader { reader in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 0) {
                            ForEach(Array(activity.log.enumerated()), id: \.offset) { line in
                                Text(line.element)
                                    .font(.system(.caption, design: .monospaced))
                                    .textSelection(.enabled)
                                    .id(line.offset)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(height: 280)
                    .onChange(of: activity.log.count) { _, count in
                        reader.scrollTo(count - 1, anchor: .bottom)
                    }
                }
            } label: {
                Text("Details").font(.headline)
            }
        }
    }
}

struct Header: View {
    let title: String
    let subtitle: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.largeTitle.bold())
            if !subtitle.isEmpty {
                Text(subtitle).foregroundStyle(.secondary)
            }
        }
    }
}
