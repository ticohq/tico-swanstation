/// @file SwanSaves.h
/// @brief Backups of the memory cards SwanStation writes in place.
#pragma once

namespace SwanSaves
{
/// Copies the game's memory card files (shared or per-game cards) into
/// backups/ beside them, keeping the last few sessions. SwanStation writes
/// these files in place while the game runs, so the copy is taken as it starts.
void BackupForSession();
} // namespace SwanSaves
