#!/usr/bin/env bash
# Builds the DLLs and assembles the host + client drop-in folders and zips.
# Usage: HOST_ADDR=1.2.3.4 ./scripts/package.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root/build/mingw}"
version="$(sed -n 's/^project(sn2lan VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
dist="$root/dist"
# No default: set HOST_ADDR so the client config is pre-filled, or leave it as
# the placeholder and let players type the address in the F8 overlay.
host_addr="${HOST_ADDR:-YOUR-HOST-IP}"

"$root/scripts/build.sh"

make_install_notes() {
    local role="$1"
    local out="$2"
    if [ "$role" = "host" ]; then
        cat > "$out" <<TXT
SN2LAN host package (${version})

Install (no UE4SS needed):
  1. Copy dwmapi.dll and config.ini into Subnautica2\\Binaries\\Win64\\
     (next to Subnautica2-Win64-Test.exe). Logs land in sn2lan.log there.
  2. Start Steam, log in, then start the game and wait for the main menu.
     Creating a session before the EOS login finishes fails with
     "Could not retrieve UniqueNetId".
  3. Click "Create multiplayer" and load your save. sn2lan.log then shows:
       net driver hook installed: StaticLoadClass -> UIpNetDriver
       control server listening on TCP 7777
       IpNetDriver listening on port 7777
  4. Publish TCP 7777 (join handshake) and UDP 7777 (game data) through frp or
     a router forward - both use remotePort 7777. frpc.toml in this package is
     ready; edit serverAddr/token first.
  5. In-game overlay: F8 shows/hides the panel, F9 joins by address. The host
     does not need it to play, but it shows the same handshake feedback.
  6. Voice chat is disabled in IP mode (no EOS voice session exists).
  7. Linux/Proton: launch with WINEDLLOVERRIDES="dwmapi=n,b" %command%
     (or set dwmapi=native,builtin in the prefix registry).
  8. Uninstall: delete dwmapi.dll and config.ini.

Every player must run the same package build: compare the first line of
sn2lan.log ("SN2LAN ${version} (build ...)").

Why not UE4SS: no UE4SS build works with game build 533 (the SN2 fork is stuck
on the May build and upstream v4.0.0 crashes after the lobby loads), so the mod
ships its own dwmapi.dll loader.
TXT
    else
        cat > "$out" <<TXT
SN2LAN client package (${version})

Install (no UE4SS needed):
  1. Copy dwmapi.dll and config.ini into Subnautica2\\Binaries\\Win64\\
     (next to Subnautica2-Win64-Test.exe). Logs land in sn2lan.log there.
  2. config.ini already points at the host: ${host_addr}
     (TCP 7777 handshake, UDP 7777 game data). If it still says
     YOUR-HOST-IP, edit the file or just type the address in the panel.
  3. Start the game, wait for the main menu, then press F8 to open the SN2LAN
     panel (or F9 to connect straight away - no mouse needed). Click Connect
     once the host has clicked "Create multiplayer"; until then the panel
     reports that the host is not ready yet.
  4. Keyboard: Tab / Up / Down select a row, Enter edits the address or port
     (Enter again to finish) or connects, Esc closes the panel. The first run
     asks you to calibrate the mouse: click the blue "+" twice; press F7 to
     redo it if clicks ever land off target.
  5. Success shows in sn2lan.log:
       handshake ok, joining <host>:7777
       manual join: ClientTravel to <host>:7777?...
  6. Identity: with a SteamID you keep your save/items; the SonarPlayerId is
     only present when the game can reach api.live.subnautica.net. Without both
     you join as a guest and cannot create a character.
  7. Voice chat is disabled in IP mode (no EOS voice session exists).
  8. Linux/Proton: launch with WINEDLLOVERRIDES="dwmapi=n,b" %command%
     (or set dwmapi=native,builtin in the prefix registry).
  9. Uninstall: delete dwmapi.dll and config.ini.

Every player must run the same package build: compare the first line of
sn2lan.log ("SN2LAN ${version} (build ...)").

Why not UE4SS: no UE4SS build works with game build 533 (the SN2 fork is stuck
on the May build and upstream v4.0.0 crashes after the lobby loads), so the mod
ships its own dwmapi.dll loader.
TXT
    fi
}

package_role() {
    local role="$1"
    local stage="$dist/SN2LAN-$role"

    rm -rf "$stage"
    mkdir -p "$stage/docs"

    cp "$build_dir/dwmapi.dll" "$stage/"
    if [ "$role" = "client" ]; then
        sed "s/@HOST@/$host_addr/g" "$root/packaging/config.client.ini" > "$stage/config.ini"
    else
        cp "$root/packaging/config.host.ini" "$stage/config.ini"
    fi
    cp "$root/packaging/config.example.ini" "$stage/"
    cp "$root/README.md" "$stage/"
    cp "$root/LICENSE" "$stage/"
    cp "$root/docs/protocol.md" "$root/docs/testing.md" "$stage/docs/"
    if [ "$role" = "host" ] && [ -f "$root/deploy/frpc.toml" ]; then
        cp "$root/deploy/frpc.toml" "$stage/"
    fi
    make_install_notes "$role" "$stage/README-install.txt"

    local archive="$dist/SN2LAN-$role-$version.zip"
    rm -f "$archive"
    if command -v zip >/dev/null 2>&1; then
        (cd "$dist" && zip -qr "$archive" "SN2LAN-$role")
    else
        (cd "$dist" && tar -czf "${archive%.zip}.tar.gz" "SN2LAN-$role")
        archive="${archive%.zip}.tar.gz"
    fi
    echo "packaged: $archive"
}

package_role host
package_role client
