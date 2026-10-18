// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where OpenGP keeps its files. A build from source uses the source tree
// (user/, and the images in ISO/ or private/); an installed package (built
// with OPENGP_PACKAGE) uses the folder its executables are in, so it can be
// unpacked and run from anywhere.
#pragma once

#include <filesystem>
#include <string>

namespace app {

// The folder of the running executable.
std::filesystem::path executable_dir();

// The folder with user/ (settings, saves) and the ISO/ and private/ folders
// searched for disc images.
std::filesystem::path home_dir();

// Keeps two copies of OpenGP from using one user folder at once (their cards,
// the game's backups and reboot files would collide): locks `user_dir`/.lock
// for the life of the process. False if another process holds it; with
// `wait`, it waits a few seconds first (for the process a reboot replaces).
bool lock_user_dir(const std::filesystem::path& user_dir, bool wait);

// user/images.toml: the disc image the installer built each game from, by game
// ID, so the game finds it wherever it is. "" if none is recorded.
std::string remembered_image(const std::filesystem::path& user_dir, const std::string& game_id);
bool remember_image(const std::filesystem::path& user_dir, const std::string& game_id, const std::string& image,
                    std::string* error);

}  // namespace app
