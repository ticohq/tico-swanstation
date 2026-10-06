/// @file SwanBios.h
/// @brief Whether a PlayStation BIOS is there, before SwanStation boots.
#pragma once

#include <string>

namespace SwanBios
{
struct Status
{
    bool ok = true;
    std::string file;   // the BIOS a US game looks for first
    std::string folder; // where the images go
};

/// A BIOS image in TicoConfig::SystemPath(): any file of a PlayStation BIOS's
/// size (512 KB, or 4 MB for ps1_rom.bin), as SwanStation finds them by
/// content. Without one the core runs its built-in OpenBIOS, which some games
/// do not work with.
Status Check();
} // namespace SwanBios
