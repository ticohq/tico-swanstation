/// @file TicoStubs.cpp
/// @brief Stub implementations for functions not needed in the tico overlay build
/// These functions are declared but not used in libretro mode with USE_SDL

// SDL Haptic stubs - declared in core/input/haptic.h when USE_SDL is defined
// but implemented in core/sdl/sdl.cpp which we don't compile for libretro

void sdl_setTorque(int port, float v) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)v;
}

void sdl_setDamper(int port, float param, float speed) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)param;
    (void)speed;
}

void sdl_setSpring(int port, float saturation, float speed) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)saturation;
    (void)speed;
}

void sdl_stopHaptic(int port) {
    // Stub - haptic not supported in this build
    (void)port;
}
