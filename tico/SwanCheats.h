/// @file SwanCheats.h
/// @brief The quick menu's cheats, run by SwanStation's own cheat engine.
#pragma once

#include <string>
#include <vector>

namespace SwanCheats
{
struct Entry
{
    std::string name;
    bool enabled = false;
};

/// Reads sdmc:/tico/cheats/<slug>/<game>.cht (RetroArch's format, cheatN_desc
/// and cheatN_code) and <game>.cheats ("# Name" then its codes): PlayStation
/// GameShark codes ("80012345 0063"). Call after the game has
/// loaded; every cheat starts off.
void Load(const std::string &gamePath, const std::string &slug);
const std::vector<Entry> &List();
/// Whether the cheats loaded are not those of the disc at @p gamePath.
bool NeedsReload(const std::string &gamePath);
void Toggle(size_t index);
void Clear();
} // namespace SwanCheats
