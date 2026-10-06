/// @file SwanCheats.cpp
/// @brief Hands the game's cheat files to SwanStation's cheat list. Each of
/// our cheats becomes one code there, its GameShark lines kept together (a
/// conditional line guards the one after it); the list is rebuilt from the
/// enabled cheats whenever one is toggled.

#include "SwanCheats.h"

#include "TicoConfig.h"
#include "TicoLogger.h"
#include "TicoUtils.h"

#include "core/cheats.h"
#include "core/system.h"

#include <cctype>
#include <cstdio>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>

namespace SwanCheats
{
namespace
{
struct Loaded
{
    Entry entry;
    std::string codes; // the cheat's GameShark lines, space-separated
};

std::vector<Loaded> s_cheats;
std::vector<Entry> s_entries;
std::string s_loadedPath;

// The name without its disc tag: "Game (USA) (Disc 2)" -> "Game (USA)".
std::string WithoutDiscTag(const std::string &name)
{
    std::string lower = name;
    for (char &c : lower)
        c = (char)std::tolower((unsigned char)c);
    for (const char *tag : {"(disc", "(disk", "(cd"})
    {
        const size_t open = lower.find(tag);
        if (open == std::string::npos)
            continue;
        const size_t close = lower.find(')', open);
        size_t start = open;
        while (start > 0 && name[start - 1] == ' ')
            start--;
        std::string out = name;
        out.erase(start, close == std::string::npos ? std::string::npos : close + 1 - start);
        return out;
    }
    return name;
}

bool Exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The disc's own cheat files, else the game's (shared by its discs).
std::string BasePath(const std::string &gamePath, const std::string &slug)
{
    std::string name = gamePath;
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    const std::string dir = "sdmc:/tico/cheats/" + slug + "/";
    TicoConfig::MakeDirs(dir);
    const std::string own = dir + name;
    if (Exists(own + ".cht") || Exists(own + ".cheats"))
        return own;
    return dir + WithoutDiscTag(name);
}

// Codes joined with + or ; become one space-separated line for the manager.
std::string JoinCodes(const std::string &text)
{
    std::string out;
    for (const char c : text)
        out += (c == '+' || c == ';' || c == ',') ? ' ' : c;
    return TicoUtils::Trim(out);
}

void ReadCht(const std::string &path, std::vector<Loaded> &out)
{
    std::ifstream file(path);
    if (!file.is_open())
        return;
    std::map<int, Loaded> byIndex;
    std::string line;
    while (std::getline(file, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = TicoUtils::Trim(line.substr(0, eq));
        std::string value = TicoUtils::Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        int index = -1;
        char field[16] = {0};
        if (sscanf(key.c_str(), "cheat%d_%15s", &index, field) != 2 || index < 0)
            continue;
        if (!strcmp(field, "desc"))
            byIndex[index].entry.name = value;
        else if (!strcmp(field, "code"))
            byIndex[index].codes = JoinCodes(value);
    }
    for (auto &item : byIndex)
    {
        if (item.second.codes.empty())
            continue;
        if (item.second.entry.name.empty())
            item.second.entry.name = "Cheat " + std::to_string(item.first + 1);
        out.push_back(item.second);
    }
}

void ReadCheats(const std::string &path, std::vector<Loaded> &out)
{
    std::ifstream file(path);
    if (!file.is_open())
        return;
    std::string line;
    while (std::getline(file, line))
    {
        const std::string text = TicoUtils::Trim(line);
        if (text.empty() || text[0] == '!')
            continue;
        if (text[0] == '#')
        {
            out.emplace_back();
            out.back().entry.name = TicoUtils::Trim(text.substr(1));
            continue;
        }
        if (out.empty())
        {
            out.emplace_back();
            out.back().entry.name = "Cheat";
        }
        Loaded &cheat = out.back();
        cheat.codes += (cheat.codes.empty() ? "" : " ") + JoinCodes(text);
    }
}

// The enabled cheats, as the core's cheat list (none: no list at all).
void Apply()
{
    std::unique_ptr<CheatList> list;
    uint32_t index = 0;
    for (const Loaded &cheat : s_cheats)
    {
        if (!cheat.entry.enabled)
            continue;
        CheatCode code;
        code.description = cheat.entry.name;
        code.enabled = true;
        if (!CheatList::ParseLibretroCheat(&code, cheat.codes.c_str()))
            continue;
        if (!list)
            list = std::make_unique<CheatList>();
        list->SetCode(index++, std::move(code));
    }
    System::SetCheatList(std::move(list));
}

void RebuildEntries()
{
    s_entries.clear();
    for (const Loaded &cheat : s_cheats)
        s_entries.push_back(cheat.entry);
}
} // namespace

void Load(const std::string &gamePath, const std::string &slug)
{
    Clear();
    s_loadedPath = gamePath;
    const std::string base = BasePath(gamePath, slug);
    std::vector<Loaded> found;
    ReadCht(base + ".cht", found);
    ReadCheats(base + ".cheats", found);
    for (Loaded &cheat : found)
    {
        CheatCode check;
        if (!CheatList::ParseLibretroCheat(&check, cheat.codes.c_str()))
        {
            LOG_WARN("CHEATS", "skipping \"%s\": unreadable codes", cheat.entry.name.c_str());
            continue;
        }
        cheat.entry.enabled = false;
        s_cheats.push_back(cheat);
    }
    RebuildEntries();
    LOG_INFO("CHEATS", "%zu for %s", s_cheats.size(), base.c_str());
}

const std::vector<Entry> &List()
{
    return s_entries;
}

bool NeedsReload(const std::string &gamePath)
{
    return gamePath != s_loadedPath;
}

void Toggle(size_t index)
{
    if (index >= s_cheats.size())
        return;
    Loaded &cheat = s_cheats[index];
    cheat.entry.enabled = !cheat.entry.enabled;
    Apply();
    RebuildEntries();
}

void Clear()
{
    const bool hadEnabled = std::any_of(s_cheats.begin(), s_cheats.end(),
                                        [](const Loaded &cheat) { return cheat.entry.enabled; });
    s_cheats.clear();
    s_entries.clear();
    if (hadEnabled && System::IsValid())
        System::SetCheatList(nullptr);
}
} // namespace SwanCheats
