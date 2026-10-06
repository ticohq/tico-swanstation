/// @file SwanBios.cpp
/// @brief Looks for a PlayStation BIOS, so a missing one gets a message
/// instead of a game that may not boot on OpenBIOS.

#include "SwanBios.h"

#include "TicoConfig.h"
#include "TicoLogger.h"

#include <dirent.h>
#include <sys/stat.h>

namespace SwanBios
{
namespace
{
constexpr long kBiosSize = 512 * 1024;
constexpr long kPs3BiosSize = 4 * 1024 * 1024; // ps1_rom.bin
} // namespace

Status Check()
{
    Status status;
    status.folder = TicoConfig::SystemPath();
    status.file = "scph5501.bin";
    status.ok = false;
    DIR *dir = opendir(status.folder.c_str());
    if (!dir)
        return status;
    while (dirent *entry = readdir(dir))
    {
        if (entry->d_name[0] == '.')
            continue;
        const std::string path = status.folder + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (st.st_size == kBiosSize || st.st_size == kPs3BiosSize)
        {
            status.ok = true;
            break;
        }
    }
    closedir(dir);
    if (!status.ok)
        LOG_WARN("BIOS", "no PlayStation BIOS in %s", status.folder.c_str());
    return status;
}
} // namespace SwanBios
