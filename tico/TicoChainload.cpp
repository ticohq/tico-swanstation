/// @file TicoChainload.cpp
#include "TicoChainload.h"

#include "TicoConfig.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include <switch/runtime/env.h>
#endif

namespace Tico
{

void ChainloadLauncher(const LogCallback &log)
{
#ifdef __SWITCH__
    const char *primary = Paths::LauncherNro;
    const char *fallback = Paths::LauncherNroFallback;
    const char *target = nullptr;

    struct stat st;
    if (stat(primary, &st) == 0)
        target = primary;
    else if (stat(fallback, &st) == 0)
        target = fallback;

    if (target)
    {
        char args[512];
        std::snprintf(args, sizeof(args), "%s --resume", target);
        envSetNextLoad(target, args);
        if (log)
            log(std::string("Chainloading back to ") + target);
    }
    else if (log)
    {
        log(std::string("No tico.nro found at ") + primary + " or " + fallback);
    }
#else
    (void)log;
#endif
}

void RelaunchSelf(int argc, char **argv, const LogCallback &log)
{
#ifdef __SWITCH__
    if (argc < 1 || !argv || !argv[0])
        return;
    // hbloader splits the argument string on spaces outside quotes
    std::string args;
    for (int i = 0; i < argc && argv[i]; ++i)
        args += (i ? " \"" : "\"") + std::string(argv[i]) + "\"";
    static char saved[2048]; // envSetNextLoad keeps the pointer until exit
    std::snprintf(saved, sizeof(saved), "%s", args.c_str());
    envSetNextLoad(argv[0], saved);
    if (log)
        log(std::string("Relaunching ") + argv[0]);
#else
    (void)argc;
    (void)argv;
    (void)log;
#endif
}

void LaunchSelf(const char *argv0, const std::vector<std::string> &args, const LogCallback &log)
{
#ifdef __SWITCH__
    if (!argv0 || !*argv0)
        return;
    // hbloader splits the argument string on spaces outside quotes
    std::string line = "\"" + std::string(argv0) + "\"";
    for (const std::string &arg : args)
        line += " \"" + arg + "\"";
    static char saved[2048]; // envSetNextLoad keeps the pointer until exit
    std::snprintf(saved, sizeof(saved), "%s", line.c_str());
    static char path[1024];
    std::snprintf(path, sizeof(path), "%s", argv0);
    envSetNextLoad(path, saved);
    if (log)
        log(std::string("Launching ") + line);
#else
    (void)argv0;
    (void)args;
    (void)log;
#endif
}

}  // namespace Tico
