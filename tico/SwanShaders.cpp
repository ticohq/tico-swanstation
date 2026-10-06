/// @file SwanShaders.cpp
/// @brief RetroArch slang presets between SwanStation's frames and the screen
/// (Vulkan only): the preset browser and parameters in Settings > Shaders,
/// and the chain the game is drawn through.

#include "SwanShaders.h"

#include "TicoLogger.h"
#include "TicoShaderChain.h"
#include "TicoSlang.h"
#include "TicoVulkan.h"
#include "json.hpp"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace OverlayConfig = SwitchFrontend::TicoConfig;

namespace SwanShaders
{
namespace
{
std::unique_ptr<TicoShaderChain> g_chain;
std::string g_activePreset = "\x01"; // forces the first load

#ifdef __SWITCH__
const char *kBuiltinShaderDir = "romfs:/shaders/";
const char *kUserShaderDir = "sdmc:/tico/shaders/";
#else
const char *kBuiltinShaderDir = "tico/shaders/";
const char *kUserShaderDir = "shaders/";
#endif

// The built-ins, with the names the menu shows for them.
const std::pair<const char *, const char *> kBuiltinShaders[] = {
    {"xbrz.slangp", "xBRZ"},
    {"eagle.slangp", "Eagle"},
    {"crt-easymode.slangp", "CRT Easy Mode"},
};

bool EndsWith(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string ShaderPreset()
{
    return OverlayConfig::GetConfigValue("shader_preset", "");
}

void SetShaderPreset(const std::string &path)
{
    OverlayConfig::SetConfigValue("shader_preset", path);
    OverlayConfig::SaveConfig();
}

// Settings from before slang presets picked one of the old GL shaders.
void MigrateShaderSetting()
{
    if (OverlayConfig::GetConfigValue("shader_preset", "\x01") != "\x01")
        return; // already chosen, "" included
    const std::pair<const char *, const char *> kOld[] = {
        {"xBRZ", "xbrz.slangp"}, {"Eagle", "eagle.slangp"}, {"CrtEasyMode", "crt-easymode.slangp"},
    };
    const std::string old = OverlayConfig::GetConfigValue("shader_type", "None");
    for (const auto &entry : kOld)
        if (old == entry.first)
            SetShaderPreset(kBuiltinShaderDir + std::string(entry.second));
}

std::string ShaderPresetLabel()
{
    const std::string preset = ShaderPreset();
    if (preset.empty())
        return std::string();
    for (const auto &builtin : kBuiltinShaders)
        if (preset == kBuiltinShaderDir + std::string(builtin.first))
            return builtin.second;
    std::string name = preset;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    return EndsWith(name, ".slangp") ? name.substr(0, name.size() - 7) : name;
}

// The browser: the user folder lists the built-ins first, every other folder
// its parent; then subfolders and presets, by name.
std::vector<OverlayUI::ShaderBrowseEntry> BrowseShaders(std::string dir)
{
    using Entry = OverlayUI::ShaderBrowseEntry;
    if (dir.empty() || dir.back() != '/')
        dir += '/';
    std::vector<Entry> entries;
    if (dir == kUserShaderDir)
    {
        entries.push_back({"> " + SwitchFrontend::OverlayTranslation::tr("emulator_builtin_shaders"),
                           kBuiltinShaderDir, true});
        entries.push_back({SwitchFrontend::OverlayTranslation::tr("emulator_none"), "", false});
    }
    else
    {
        std::string parent = kUserShaderDir;
        if (dir != kBuiltinShaderDir)
        {
            const std::string d = dir.substr(0, dir.size() - 1);
            const size_t slash = d.find_last_of('/');
            if (slash != std::string::npos)
                parent = d.substr(0, slash + 1);
        }
        entries.push_back({"..", parent, true});
    }
    if (dir == kBuiltinShaderDir)
    {
        for (const auto &builtin : kBuiltinShaders)
            entries.push_back({builtin.second, dir + builtin.first, false});
        return entries;
    }

    std::vector<Entry> dirs, files;
    if (DIR *d = opendir(dir.c_str()))
    {
        while (struct dirent *e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.')
                continue;
            const std::string path = dir + name;
            bool isDir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN)
            {
                struct stat st;
                isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (isDir)
                dirs.push_back({name + "/", path + "/", true});
            else if (EndsWith(name, ".slangp"))
                files.push_back({name.substr(0, name.size() - 7), path, false});
        }
        closedir(d);
    }
    auto byName = [](const Entry &a, const Entry &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());
    return entries;
}

// Parameter overrides per preset, in swanstation.jsonc's shader_parameters.
nlohmann::json ShaderParameterOverrides()
{
    const std::string text = OverlayConfig::GetConfigJson("shader_parameters");
    nlohmann::json j = text.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

void SaveShaderParameterOverrides(const nlohmann::json &j)
{
    OverlayConfig::SetConfigJson("shader_parameters", j.dump());
    OverlayConfig::SaveConfig();
}

// A preset just loaded: start from its defaults, then the saved overrides.
void OnShaderLoaded()
{
    if (!g_chain)
        return;
    g_chain->ResetParameters();
    const nlohmann::json overrides = ShaderParameterOverrides();
    const auto it = overrides.find(ShaderPreset());
    if (it == overrides.end() || !it->is_object())
        return;
    for (const auto &param : it->items())
        if (param.value().is_number())
            g_chain->SetParameter(param.key(), param.value().get<float>());
}

void SetShaderParameter(const std::string &id, float value)
{
    if (!g_chain)
        return;
    g_chain->SetParameter(id, value);
    nlohmann::json overrides = ShaderParameterOverrides();
    nlohmann::json &preset = overrides[ShaderPreset()];
    if (!preset.is_object())
        preset = nlohmann::json::object();
    for (const TicoSlang::Parameter &p : g_chain->Parameters())
    {
        if (p.id != id)
            continue;
        const float step = p.step > 0.0f ? p.step : 0.01f;
        if (std::fabs(value - p.initial) < step * 0.5f)
            preset.erase(id);
        else
            preset[id] = value;
    }
    if (preset.empty())
        overrides.erase(ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

void ResetShaderParameters()
{
    if (g_chain)
        g_chain->ResetParameters();
    nlohmann::json overrides = ShaderParameterOverrides();
    overrides.erase(ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

void RegisterShaderMenu()
{
    OverlayUI::ShaderCallbacks callbacks;
    callbacks.preset_label = [] { return ShaderPresetLabel(); };
    callbacks.browse_start = [] {
        const std::string preset = ShaderPreset();
        const size_t slash = preset.find_last_of('/');
        return slash == std::string::npos ? std::string(kUserShaderDir) : preset.substr(0, slash + 1);
    };
    callbacks.browse = [](const std::string &dir) { return BrowseShaders(dir); };
    callbacks.select = [](const std::string &path) { SetShaderPreset(path); };
    callbacks.parameters = [] {
        std::vector<OverlayUI::ShaderParameter> out;
        if (g_chain)
            for (const TicoSlang::Parameter &p : g_chain->Parameters())
                out.push_back({p.id, p.description, p.value, p.minimum, p.maximum, p.step});
        return out;
    };
    callbacks.set_parameter = [](const std::string &id, float value) { SetShaderParameter(id, value); };
    callbacks.reset_parameters = [] { ResetShaderParameters(); };
    OverlayUI::SetShaderCallbacks(std::move(callbacks));
}

// Loads the preset the settings name once it differs from the active one.
// Compiling can take a while on the Switch, so the frame before it shows a
// toast instead of the screen just freezing.
void ApplyShaderPreset()
{
    const std::string wanted = ShaderPreset();
    if (!g_chain || wanted == g_activePreset)
        return;
    std::string announced;
    if (announced != wanted && !wanted.empty())
    {
        announced = wanted;
        OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_loading_shader"),
                             OverlayUI::ToastCorner::TopRight);
        return;
    }
    announced.clear();
    std::string error;
    if (g_chain->LoadPreset(wanted, error))
    {
        g_activePreset = wanted;
        OnShaderLoaded();
        return;
    }
    LOG_ERROR("SHADER", "Cannot load %s: %s", wanted.c_str(), error.c_str());
    const std::string firstLine = error.substr(0, error.find('\n'));
    OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_shader_failed") + ": " +
                             firstLine.substr(0, 80),
                         OverlayUI::ToastCorner::TopRight);
    // Keep showing (and saving) what actually runs.
    if (g_activePreset == "\x01")
        g_activePreset.clear();
    SetShaderPreset(g_activePreset);
}


} // namespace

bool Init()
{
    g_chain = std::make_unique<TicoShaderChain>();
    if (!g_chain->Init())
    {
        LOG_ERROR("SHADER", "Shader chain initialization failed");
        g_chain.reset();
        return false;
    }
    g_activePreset = "\x01";
    MigrateShaderSetting();
    RegisterShaderMenu();
    return true;
}

void Update()
{
    ApplyShaderPreset();
}

void Shutdown()
{
    OverlayUI::SetShaderCallbacks({});
    g_chain.reset();
    TicoSlang::Shutdown();
}

TicoShaderChain *Chain()
{
    return g_chain.get();
}
} // namespace SwanShaders
