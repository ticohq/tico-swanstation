/// @file SwanEntry.cpp
/// @brief Process entry point for the tico-integrated SwanStation NRO: wires a
/// log sink and the SwanStation runtime into the generic Tico::Main driver.

#include "SwanRuntime.h"
#include "TicoLogger.h"
#include "TicoMain.h"

#ifdef __SWITCH__
#include <switch.h>

extern "C"
{
    u32 __NvOptimusEnablement = 1;
    u32 __NvDeveloperOption = 1;
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;
}
#endif

int main(int argc, char *argv[])
{
    // Route the agnostic layer's log lines through the project macro logger.
    Tico::LogCallback log = [](const std::string &line) {
        LOG_INFO("HOME", "%s", line.c_str());
    };

    Tico::SwanRuntime runtime(log);
    Tico::Main app(runtime, log);
    return app.Run(argc, argv);
}
