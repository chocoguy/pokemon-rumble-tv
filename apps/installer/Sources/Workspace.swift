import Foundation

/// Where the installer keeps everything, in one folder: ~/Library/Application Support/Rumble TV
/// Installer. Not inside the app: writing there breaks its signature, a quarantined copy runs read-only,
/// and replacing the app (a new download) would lose it all.
struct Workspace: Sendable {
    let root: URL

    init(root: URL? = nil) {
        self.root = root ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Rumble TV Installer", isDirectory: true)
    }

    /// The git checkout of the release being built, with its submodule.
    var source: URL { root.appendingPathComponent("source", isDirectory: true) }
    /// tools/tvos/port's --work-dir: the imported game files and the builds, kept across updates.
    var work: URL { root.appendingPathComponent("work", isDirectory: true) }
    var tools: URL { root.appendingPathComponent("tools", isDirectory: true) }
    var logs: URL { root.appendingPathComponent("logs", isDirectory: true) }
    var stateFile: URL { root.appendingPathComponent("state.json") }
    var releasesCache: URL { root.appendingPathComponent("cache/releases.json") }
    var port: URL { source.appendingPathComponent("tools/tvos/port") }
    /// What Free Up Space removes: tools/tvos/port's Xcode project and build (its derived data too), which
    /// the next install makes again. The imported game files, the source and CMake stay.
    var rebuildable: [URL] { [work.appendingPathComponent("xcode", isDirectory: true)] }

    /// Bytes on disk, everything in the folder.
    func size() -> Int64 {
        guard let files = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.totalFileAllocatedSizeKey],
                                                         options: [], errorHandler: { _, _ in true }) else {
            return 0
        }
        var total: Int64 = 0
        for case let file as URL in files {
            total += Int64((try? file.resourceValues(forKeys: [.totalFileAllocatedSizeKey]))?.totalFileAllocatedSize ?? 0)
        }
        return total
    }

    func load() -> InstallerState {
        guard let data = try? Data(contentsOf: stateFile),
              let state = try? JSONDecoder.installer.decode(InstallerState.self, from: data) else {
            return InstallerState()
        }
        return state
    }

    func save(_ state: InstallerState) {
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        if let data = try? JSONEncoder.installer.encode(state) {
            try? data.write(to: stateFile, options: .atomic)
        }
    }
}

/// Where the game goes. This port builds for the Apple TV only.
enum Platform: String, Codable, CaseIterable, Identifiable, Sendable {
    case tvos

    var id: String { rawValue }
    var title: String { "Apple TV" }
    var symbol: String { "appletv" }
    /// devicectl's name for the platform.
    var devicePlatform: String { "tvOS" }
    /// xcodebuild's SDK, and the name -downloadPlatform takes.
    var sdk: String { "appletvos" }
    var downloadName: String { "tvOS" }
}

/// The player's game files: the WAD they installed in Dolphin (Tools > Install WAD), which Dolphin keeps
/// decrypted in its Wii folder, or a folder of the decrypted .app contents. tools/tvos/port imports them.
enum GameFiles {
    /// Pokémon Rumble (USA): title 00010001/57505345 ("WPSE").
    static let titlePath = "Wii/title/00010001/57505345/content"
    static var dolphin: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Dolphin", isDirectory: true)
    }

    /// The folder of contents in `chosen` (empty: Dolphin's own folder), or nil if there are none.
    static func content(in chosen: String) -> URL? {
        let folder = chosen.isEmpty ? dolphin : URL(fileURLWithPath: chosen, isDirectory: true)
        let files = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        if files.contains(where: { $0.lowercased().hasSuffix(".app") }) {
            return folder
        }
        for base in [folder, folder.deletingLastPathComponent()] {
            let content = base.appendingPathComponent(titlePath, isDirectory: true)
            if FileManager.default.fileExists(atPath: content.appendingPathComponent("title.tmd").path)
                || ((try? FileManager.default.contentsOfDirectory(atPath: content.path)) ?? []).contains(where: { $0.hasSuffix(".app") }) {
                return content
            }
        }
        return nil
    }
}

/// Where the game is installed: an Apple TV.
struct Install: Codable, Identifiable, Hashable, Sendable {
    var platform: Platform
    /// devicectl's identifier.
    var deviceId: String?
    var deviceName: String
    var version: String?
    var commit: String?
    var installedAt: Date?
    /// When the device's development profile stops the app opening.
    var expires: Date?
    /// The last update didn't reach it (it was off, or away): try again.
    var pending: Bool = false

    var id: String { platform.rawValue + ":" + (deviceId ?? "") }
}

/// What the installer remembers (state.json).
struct InstallerState: Codable, Sendable {
    /// The game files the player chose: Dolphin's user folder or a folder of contents. Empty: Dolphin's
    /// own folder (~/Library/Application Support/Dolphin).
    var game: String = ""
    /// The release tag the source is at, once fetched.
    var tag: String?
    var installs: [Install] = []

    mutating func record(_ install: Install) {
        if let index = installs.firstIndex(where: { $0.id == install.id }) {
            installs[index] = install
        } else {
            installs.append(install)
        }
    }
}

extension JSONEncoder {
    static var installer: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return encoder
    }
}

extension JSONDecoder {
    static var installer: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }
}
