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

    // ---- data packs: the profile card (docs/mod-layer.md "Data packs: save profiles", "Leaving profiles") ----
    // Called by the runtime when the game's executable is loaded, right after the memory card folders were set from it and before the
    // game's first card call. Decides which card the game plays from (mods/profile.h, select()):
    //   - at least one data pack is active in the session: the profile card (<pref folder>/profile), built or updated, and the memory
    //     card root is pointed at its mc0;
    //   - no data pack, but the profile has a save and the player has not chosen "Back to my card": the profile card still, with every
    //     field the packs wrote put back to its "before" bytes (the player's own changes stay);
    //   - otherwise the base card next to the executable, exactly as without mods; nothing is created, read or written.
    // `elfPath` is the game executable: the checksum table the game's save integrity needs is read from it (only when the profile is used).
    void selectCard(const std::string &elfPath);
    // The Mods tab's line saying which card the game is using, and why ("" without a manager).
    std::string cardLine();
    // "Keep this card": true when the profile has a save to copy to the base card.
    bool canKeepCard();
    // Copies the profile's save to the base card (after backing the base card's files up). One sentence for the player.
    std::string keepCard();
    // "Back to my card": true while no data pack is on (as the Mods tab shows it now), the profile has a save and the player has not
    // chosen it already. It takes effect at the next start.
    bool canBackToOwnCard();
    // Records the choice (after the menu asked "are you sure"). One sentence for the player.
    std::string backToOwnCard();

    struct DropOutcome
    {
        bool handled = false; // the manager exists and looked at the file
        bool ok = false;      // it was installed
        std::string message;  // one sentence for the player
        // A save import (task s4) only: the file held several saves of the game, so nothing was imported yet. `choices` are the labels to
        // offer ("USA", "Japan", "Europe": the question's text names the folders), `folders` the folder names in the same order; call importSave(path, folders[i]) with the one picked.
        std::vector<std::string> choices;
        std::vector<std::string> folders;
        std::string path; // the file, for that second call
        std::string modKey; // the mod that was installed (an import, or a dropped .fprmod): the Mods tab selects it
    };
    // The player dropped a file (UTF-8 path) on the window: installs it if it is a good .fprmod. Logs the result.
    DropOutcome installDropped(const std::string &utf8Path);

    // ---- importing a community save as a data pack (task s4; mods/save_import.h, docs/formats/fprmod.md "Importing a save") ----
    // True when this build can import (it has a mod manager). The Mods tab's "Import a save..." button is shown only then.
    bool canImportSave();
    // Turns the save in `utf8Path` (a .max, .psu, .xps/.sps, .cbs, .psv, a .ps2 card image, a raw save...) into an installed, switched-on data
    // pack, comparing it with the player's own card (or, when a profile has a save, with the profile without its packs). `folder` names
    // the save to take when the file holds several saves of the game (leave it empty the first time; see DropOutcome::choices).
    // Logs what it did. The result is shown on the Mods tab.
    DropOutcome importSave(const std::string &utf8Path, const std::string &folder = std::string());
    // What a file dropped on the window gets: a .fprmod is installed (installDropped), anything else is tried as a save (importSave).
    DropOutcome handleDropped(const std::string &utf8Path);
}
