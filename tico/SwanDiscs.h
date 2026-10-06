/// @file SwanDiscs.h
/// @brief Multi-disc discovery for the overlay's Change Disc menu.
#pragma once

#include <string>
#include <vector>

struct DiscEntry
{
    std::string displayName;
    std::string romPath;
};

/// The discs of the game at @p currentPath, sorted by disc number; empty when
/// it is a single-disc game. Paths use forward slashes.
std::vector<DiscEntry> ScanDiscs(std::string currentPath);

/// @p path with backslashes turned into forward slashes, as ScanDiscs reports.
std::string NormalizeDiscPath(const std::string &path);
