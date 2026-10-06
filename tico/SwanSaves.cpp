/// @file SwanSaves.cpp
/// @brief Backs up the memory cards SwanStation writes in place.

#include "SwanSaves.h"

#include "TicoLogger.h"
#include "TicoSafeFile.h"

#include "core/memory_card.h"
#include "core/pad.h"
#include "core/system.h"

#include <string>

namespace SwanSaves
{
namespace
{
constexpr int kKeep = 3;
} // namespace

void BackupForSession()
{
    if (!System::IsValid())
        return;
    // Shared and per-game cards are files the core writes itself; memory card
    // 1 kept by the frontend (<game>.mcd) has no file here and is backed up
    // as TicoCore writes it.
    for (uint32_t slot = 0; slot < 2; ++slot)
    {
        const MemoryCard *card = g_pad.GetMemoryCard(slot);
        if (!card || card->GetFilename().empty() || !TicoSafeFile::Exists(card->GetFilename()))
            continue;
        TicoSafeFile::BackupCopy(card->GetFilename(), kKeep);
        LOG_INFO("SAVES", "backed up %s", card->GetFilename().c_str());
    }
}
} // namespace SwanSaves
