/// @file TicoChainload.h
/// @brief Chainload back to the tico launcher NRO. Emulator-agnostic.
#pragma once

#include "TicoLogger.h"

#include <string>
#include <vector>

namespace Tico
{

/// Queue the tico launcher (sdmc:/switch/tico.nro, with fallback) as the next
/// homebrew to load via envSetNextLoad. Call after the runtime has shut down.
/// No-op off Switch.
void ChainloadLauncher(const LogCallback &log = {});

/// Queue this NRO again (argv[0]) with the same arguments, to start the game
/// afresh. Call after the runtime has shut down. No-op off Switch.
void RelaunchSelf(int argc, char **argv, const LogCallback &log = {});

/// Queue this NRO (@p argv0) with @p args: a game chosen in its library, or
/// none to come back to the library. No-op off Switch.
void LaunchSelf(const char *argv0, const std::vector<std::string> &args, const LogCallback &log = {});

}  // namespace Tico
