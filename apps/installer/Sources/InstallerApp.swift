// Rumble TV Installer: builds Pokémon Rumble TV from the player's own game files (the WAD they
// installed in Dolphin), at a GitHub release tag, and installs it on their Apple TVs; updates them all
// when a new release comes out. Distributed signed and notarized (tools/release.sh). It doesn't update
// itself: a new major version of the game is a new installer, which the player downloads; until then it
// says so, and game updates wait.

import SwiftUI

@main
struct InstallerApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        Window("Rumble TV Installer", id: "main") {
            ContentView()
                .environmentObject(model)
                .task { model.start() }
        }
    }
}
