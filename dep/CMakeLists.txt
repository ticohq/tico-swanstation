
# Force disable LTO for dependencies to fix objcopy
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

add_subdirectory(glad)
add_subdirectory(stb)
add_subdirectory(zlib)
add_subdirectory(zstd)
add_subdirectory(lzma)
add_subdirectory(libchdr)
add_subdirectory(xxhash)
set(USE_EVDEV OFF)

option(ENABLE_VULKAN "Enable Vulkan renderer" ON)
if(ENABLE_VULKAN)
  add_subdirectory(glslang)
  add_subdirectory(vulkan-loader)
endif()
add_subdirectory(libretro-common)

if(${CPU_ARCH} STREQUAL "aarch32")
  set(CPU_ARCH_ARM32 ON)
  add_subdirectory(vixl)
elseif(${CPU_ARCH} STREQUAL "aarch64")
  set(CPU_ARCH_ARM64 ON)
  add_subdirectory(vixl)
endif()

