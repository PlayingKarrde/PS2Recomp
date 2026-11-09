#pragma once

// The game's side of the mod manager (src/lib/mods): where the mods live, the one manager the host and the overlay share, and
// installing a file the player dropped on the window. The manager itself (mod_manager.h) knows nothing about the game's
// folders; this file decides them.
//
// Where things are (the "pref folder" is the folder of settings.cfg: SDL's preference path for the app, or the folder of the
// file PS2X_SETTINGS names, which is how the tests keep a run away from a player's own mods):
//     <pref folder>/mods/          the .fprmod packages (PS2X_MODS_DIR=<folder> names another folder)
//     <pref folder>/mods.toml      which mods are on, their order and their settings
//     Windows  %APPDATA%\FPWR-Recomp\FPWR\mods
//     Linux    ~/.local/share/FPWR-Recomp/FPWR/mods
//     macOS    ~/Library/Application Support/FPWR-Recomp/FPWR/mods
//
// Nothing here includes SDL, so it can be called from anywhere in the runtime. Without PS2X_BUILD_MODS the functions do nothing.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::mods
{
    class Manager;
}

namespace TexPack
{
    class FileSource;
}

namespace ps2x_host_mods
{
    // Starts the manager: reads mods.toml, scans the mods folder, logs one line. Call once, after the settings file is read.
    // A second call does nothing.
    void initialize();

    // The manager, or nullptr when this build has none or it could not start (then the game runs without mods).
    ps2x::mods::Manager *manager();

    // The mods folder as text for the overlay's "Mods folder" line; empty when there is no manager.
    std::string modsFolder();

    // The style sheets of the theme mods that were on when the game started, as virtual names ("mod:<id>/<path>"), in load order.
    // Empty without a manager. The overlay adds them after its own theme.
    std::vector<std::string> themeStyleSheets();
    // The enabled mods' files as the texture pack store reads them (gs_tex_pack.h): the manager's virtual lookup, for the mods that
    // were on when the game started. Null without a manager. initialize() registers it with TexPack::SetDefaultSource.
    std::shared_ptr<TexPack::FileSource> texturePackSource();
    // Reads a file by virtual name ("mod:<id>/<path>"): the overlay's file interface uses it for those style sheets and the
    // pictures they and the Mods tab show. False when there is no such file.
    bool readVirtualFile(const std::string &name, std::vector<uint8_t> &bytes);

    struct DropOutcome
    {
        bool handled = false; // the manager exists and looked at the file
        bool ok = false;      // it was installed
        std::string message;  // one sentence for the player
    };
    // The player dropped a file (UTF-8 path) on the window: installs it if it is a good .fprmod. Logs the result.
    DropOutcome installDropped(const std::string &utf8Path);
}
