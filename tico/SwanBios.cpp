/// @file SwanBios.cpp
/// @brief Looks for a PlayStation BIOS as SwanStation does, so a missing or
/// unrecognised one gets a message instead of a game that may not boot on
/// OpenBIOS.

#include "SwanBios.h"

#include "TicoConfig.h"
#include "TicoLogger.h"

#include "core/bios.h"

#include <dirent.h>
#include <sys/stat.h>

namespace SwanBios
{

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
        // the sizes HostInterface::FindBIOSImageInDirectory takes
        if (st.st_size != BIOS::BIOS_SIZE && st.st_size != BIOS::BIOS_SIZE_PS2 && st.st_size != BIOS::BIOS_SIZE_PS3)
            continue;
        // ... and, like it, only a dump it knows (any region will do)
        const std::optional<BIOS::Image> image = BIOS::LoadImageFromFile(path.c_str());
        if (image && BIOS::GetImageInfo(*image))
        {
            status.ok = true;
            status.unknown.clear();
            break;
        }
        if (status.unknown.empty())
            status.unknown = entry->d_name;
    }
    closedir(dir);
    if (!status.ok)
        LOG_WARN("BIOS", "no known PlayStation BIOS in %s%s%s", status.folder.c_str(),
                 status.unknown.empty() ? "" : ", unknown: ", status.unknown.c_str());
    return status;
}

} // namespace SwanBios
