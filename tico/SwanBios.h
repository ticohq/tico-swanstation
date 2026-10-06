/// @file SwanBios.h
/// @brief Whether SwanStation will find a PlayStation BIOS, before it boots.
#pragma once

#include <string>

namespace SwanBios
{
struct Status
{
    bool ok = true;
    std::string unknown; // a file of a BIOS's size the core does not recognise
    std::string file;    // the BIOS a US game looks for first
    std::string folder;  // where the images go
};

/// A BIOS image in TicoConfig::SystemPath() that SwanStation recognises, the
/// way it looks for one at boot: a file of a BIOS's size whose contents are a
/// known dump. Without one the core quietly runs its built-in OpenBIOS, which
/// some games do not work with.
Status Check();
} // namespace SwanBios
