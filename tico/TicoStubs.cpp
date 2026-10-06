/// @file TicoStubs.cpp
/// @brief Stubs for POSIX functions not available on Switch

extern "C" {

// POSIX stubs needed by imgui (Platform_OpenInShellFn_DefaultImpl)
int fork(void) { return -1; }
int execvp(const char *file, char *const argv[]) { return -1; }
int waitpid(int pid, int *status, int options) { return -1; }

} // extern "C"

// The core builds stb_image without its file functions (STBI_NO_STDIO); the
// overlay's images and the shader presets' textures load through this one.
#include "deps/stb/stb_image.h"

#include <cstdio>
#include <vector>

stbi_uc *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp)
{
    FILE *file = std::fopen(filename, "rb");
    if (!file)
        return nullptr;
    std::vector<stbi_uc> data;
    stbi_uc chunk[16384];
    size_t read;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
        data.insert(data.end(), chunk, chunk + read);
    std::fclose(file);
    if (data.empty())
        return nullptr;
    return stbi_load_from_memory(data.data(), static_cast<int>(data.size()), x, y, comp, req_comp);
}
