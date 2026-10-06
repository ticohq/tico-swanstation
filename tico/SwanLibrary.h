/// @file SwanLibrary.h
/// @brief The game list shown when SwanStation starts without a game.
#pragma once

#include <functional>
#include <string>

namespace SwanLibrary
{
/// Registers the overlay's library (the games in tico's ROM folders and the
/// module's own) and its Settings > Library folder editor. @p launch gets the
/// chosen game and its console slug.
void Register(std::function<void(const std::string &path, const std::string &slug)> launch);
void Unregister();
} // namespace SwanLibrary
