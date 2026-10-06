#!/bin/bash

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64

echo "=== Building SwanStation NRO with Tico Overlay ==="

# Include devkitA64 toolchain
source $DEVKITPRO/devkitA64/base_tools 2>/dev/null || true

PORTLIBS=$DEVKITPRO/portlibs/switch
LIBNX=$DEVKITPRO/libnx

# Project root
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build_tico"
TICO_DIR="$ROOT_DIR/tico"

# NACP version, and the version RetroAchievements sees in the User-Agent
APP_VERSION="3.0.0"

# Rendering is Vulkan on Mesa's NVK, linked statically (a loaderless
# libvulkan.a). Point MESA_NVK_DIR at builddir-switch of a mesa-switch tree;
# without one, the switch-dev image's Horizon-native NVK in portlibs is used.
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"
NVK_ARCHIVE_SRC="$MESA_NVK_DIR/src/nouveau/vulkan/libvulkan.a"
NVK_DEPS="-ldrm_nouveau -lexpat"
if [ ! -f "$NVK_ARCHIVE_SRC" ]; then
    NVK_ARCHIVE_SRC="$PORTLIBS/lib/libvulkan.a"
    # no libdrm_nouveau in that build; see its vulkan.pc
    NVK_DEPS="-lexpat"
fi
if [ ! -f "$NVK_ARCHIVE_SRC" ]; then
    echo "Error: no NVK libvulkan.a (set MESA_NVK_DIR)"
    exit 1
fi
echo "NVK: $NVK_ARCHIVE_SRC"

# ============================================================
# Step 1: Build the SwanStation core (CMake), standalone
# ============================================================
# TICO_STANDALONE leaves the libretro glue out: tico/swan/ drives the core.
# The core's objects are kept between builds; SWAN_CLEAN=1 starts over.
echo "--- Step 1: Building the SwanStation core ---"

CORE_BUILD="$ROOT_DIR/build_core"
if [ "${SWAN_CLEAN:-0}" = "1" ]; then
    rm -rf "$CORE_BUILD"
fi
CORE_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -O2 -g -ffunction-sections -fdata-sections"
CORE_FLAGS="$CORE_FLAGS -D__SWITCH__ -DHAVE_LIBNX -ffile-prefix-map=$ROOT_DIR/="
cmake -S "$ROOT_DIR" -B "$CORE_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DPLATFORM=libnx \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DTICO_STANDALONE=ON -DLIBRETRO_COMMON_NO_MMAP=ON -DENABLE_MMAP_FASTMEM=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="$CORE_FLAGS" -DCMAKE_CXX_FLAGS="$CORE_FLAGS" > /dev/null || exit 1
cmake --build "$CORE_BUILD" --target core || exit 1

# every static library the core target pulled in
CORE_LIBS=($(find "$CORE_BUILD" -name "*.a" | sort))
echo "Core libraries: ${#CORE_LIBS[@]}"

# The frontend's files that include core headers must see the core's own
# definitions (its struct layouts depend on them): take them from a core TU.
CORE_DEFS=$(python3 - "$CORE_BUILD/compile_commands.json" <<'PYEOF'
import json, shlex, sys
for entry in json.load(open(sys.argv[1])):
    if entry["file"].endswith("src/core/system.cpp"):
        args = shlex.split(entry["command"])
        print(" ".join(a for a in args if a.startswith("-D")))
        break
PYEOF
)
echo "Core definitions: $CORE_DEFS"

# ============================================================
# Step 1b: glslang (compiles slang shaders to SPIR-V at runtime)
# ============================================================
# Kept outside build_tico, which is wiped every run: glslang only needs
# rebuilding when the submodule moves. Don't pass CMAKE_CXX_FLAGS here: it
# replaces the toolchain's -mtp=soft, and glslang's thread_locals then read a
# null thread pointer and crash on the first shader compile.
GLSLANG_BUILD="$ROOT_DIR/build_glslang_nx"
echo "--- Step 1b: Building glslang ---"
cmake -S "$TICO_DIR/deps/glslang" -B "$GLSLANG_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF \
    -DBUILD_EXTERNAL=OFF -DENABLE_SPVREMAPPER=OFF -DBUILD_SHARED_LIBS=OFF \
    -DGLSLANG_ENABLE_INSTALL=OFF > /dev/null || exit 1
cmake --build "$GLSLANG_BUILD" || exit 1
GLSLANG_LIBS=(
    "$GLSLANG_BUILD/glslang/libglslang.a"
    "$GLSLANG_BUILD/glslang/libglslang-default-resource-limits.a"
)

# ============================================================
# Step 2: Compile Tico overlay sources
# ============================================================
echo "--- Step 2: Compiling Tico overlay sources ---"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

CC="${DEVKITA64}/bin/aarch64-none-elf-gcc"
CXX="${DEVKITA64}/bin/aarch64-none-elf-g++"

COMMON_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -O2 -g"
COMMON_FLAGS="$COMMON_FLAGS -ffunction-sections -fdata-sections -D__SWITCH__ -DHAVE_LIBNX"
# SWAN_DEBUG=1 keeps the log (the frontend's and the core's), written to
# sdmc:/tico/debug/swanstation.txt; release builds leave it out.
if [ "${SWAN_DEBUG:-0}" != "1" ]; then
    COMMON_FLAGS="$COMMON_FLAGS -DDISABLE_LOGGING"
fi
COMMON_FLAGS="$COMMON_FLAGS -DLIBARCHIVE_STATIC -DVK_USE_PLATFORM_VI_NN -DTICO_APP_VERSION=\"$APP_VERSION\""
COMMON_FLAGS="$COMMON_FLAGS -I$LIBNX/include -I$PORTLIBS/include -I$PORTLIBS/include/SDL2"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR -I$TICO_DIR/deps"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR/deps/vulkan-headers"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR/deps/glslang -I$TICO_DIR/deps/SPIRV-Reflect"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/dep/libretro-common/include"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/rcheevos/include -DRC_CLIENT_SUPPORTS_HASH"

CXXFLAGS="$COMMON_FLAGS -std=gnu++17 -fvisibility-inlines-hidden -fno-rtti -fno-exceptions"

# The frontend files that talk to the core: its headers and definitions
SWAN_FLAGS="$CORE_DEFS -I$ROOT_DIR/src -I$CORE_BUILD/src -I$ROOT_DIR/dep/vulkan-loader/include"
SWAN_FLAGS="$SWAN_FLAGS -I$ROOT_DIR/dep/xxhash/include -I$ROOT_DIR/dep/stb/include"

# Tico C++ sources
# these include the core's headers (SWAN_FLAGS)
SWAN_SOURCES=(
    "$TICO_DIR/SwanEntry.cpp"
    "$TICO_DIR/SwanRuntime.cpp"
    "$TICO_DIR/SwanBios.cpp"
    "$TICO_DIR/SwanCheats.cpp"
    "$TICO_DIR/SwanDiscs.cpp"
    "$TICO_DIR/SwanLibrary.cpp"
    "$TICO_DIR/SwanSaves.cpp"
    "$TICO_DIR/SwanShaders.cpp"
    "$TICO_DIR/TicoMain.cpp"
    "$TICO_DIR/TicoChainload.cpp"
    "$TICO_DIR/TicoCore.cpp"
    "$TICO_DIR/swan/SwanHost.cpp"
    "$ROOT_DIR/src/libretro/libretro_game_settings.cpp"
)

TICO_SOURCES=(
    "$TICO_DIR/TicoGL.cpp"
    "$TICO_DIR/UsbStorage.cpp"
    "$TICO_DIR/TicoVulkan.cpp"
    "$TICO_DIR/TicoShaderChain.cpp"
    "$TICO_DIR/TicoSlang.cpp"
    "$TICO_DIR/TicoStubs.cpp"
    "$TICO_DIR/overlay/imgui_overlay.cpp"
    "$TICO_DIR/overlay/overlay_ui.cpp"
    "$TICO_DIR/overlay/ra_alerts.cpp"
    "$TICO_DIR/overlay/tico_config.cpp"
    "$TICO_DIR/overlay/translation_manager.cpp"
)

# NVK's libvulkan.a exports the vk* entry points, so no loader (volk) here.
TICO_C_SOURCES=(
    "$TICO_DIR/deps/SPIRV-Reflect/spirv_reflect.c"
)

# ImGui sources
IMGUI_DIR="$TICO_DIR/deps/imgui"
IMGUI_SOURCES=(
    "$IMGUI_DIR/imgui.cpp"
    "$IMGUI_DIR/imgui_draw.cpp"
    "$IMGUI_DIR/imgui_tables.cpp"
    "$IMGUI_DIR/imgui_widgets.cpp"
    "$IMGUI_DIR/imgui_demo.cpp"
    "$IMGUI_DIR/backends/imgui_impl_vulkan.cpp"
    "$IMGUI_DIR/backends/imgui_impl_opengl3.cpp"
)

# ImGui's OpenGL backend loads through the core's glad (no dlopen on Horizon)
IMGUI_FLAGS="-I$IMGUI_DIR -I$IMGUI_DIR/backends -I$ROOT_DIR/dep/glad/include"
IMGUI_FLAGS="$IMGUI_FLAGS -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM -include glad.h"

# rcheevos sources
RCHEEVOS_DIR="$ROOT_DIR/rcheevos"
RCHEEVOS_SOURCES=($(find "$RCHEEVOS_DIR/src" -type f -name "*.c" ! -name "rc_client_external.c" 2>/dev/null || true))

TICO_OBJS=()

# Compile the sources that drive the core
for src in "${SWAN_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $SWAN_FLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile Tico C++ sources (TicoCore.h brings in the core's headers too)
for src in "${TICO_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $SWAN_FLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# The Switch exception entry that hands fastmem faults to the core: an
# object of its own, so it overrides libnx's weak __libnx_exception_entry
ASM_SOURCES=("$ROOT_DIR/src/common/switch_exception_entry.S")
for src in "${ASM_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.S}.o)"
    echo "  AS  $src"
    $CC -march=armv8-a+crc+crypto -fPIE -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error assembling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile SPIRV-Reflect
for src in "${TICO_C_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.c}.o)"
    echo "  CC  $src"
    $CC $COMMON_FLAGS -std=gnu11 -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile rcheevos sources
for src in "${RCHEEVOS_SOURCES[@]}"; do
    # Since rcheevos has nested dirs, we flatten by using `basename` but to avoid collisions
    # we can just use the hash of the file or relative path. For simplicity, rcheevos files 
    # mostly have unique names. Let's prepend part of path to avoid collisions
    filename=$(basename "$src")
    dirprefix=$(basename $(dirname "$src"))
    obj="$BUILD_DIR/rc_${dirprefix}_${filename%.c}.o"
    echo "  CC  $src"
    $CC $COMMON_FLAGS -std=gnu11 -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile ImGui sources
for src in "${IMGUI_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

echo "Compiled ${#TICO_OBJS[@]} tico/imgui objects"

# ============================================================
# Step 3: Link everything into ELF
# ============================================================
echo "--- Step 3: Linking swanstation_tico.elf ---"

ELF_OUTPUT="$BUILD_DIR/swanstation_tico.elf"

LINK_FLAGS="-specs=$LIBNX/switch.specs -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
LINK_FLAGS="$LINK_FLAGS -Wl,--gc-sections -Wl,-Map=$BUILD_DIR/swanstation_tico.map"

# tico/deps/usbhsfs first: libusbhsfs (FAT/exFAT) that also reads NTFS through usbntfs
LINK_LIBS="-L$TICO_DIR/deps/usbhsfs/lib -L$PORTLIBS/lib -L$LIBNX/lib"
LINK_LIBS="$LINK_LIBS -lSDL2_mixer -lmpg123 -lmodplug -lopusfile -lopus -lvorbisidec -logg -lSDL2"

# Mesa's EGL and desktop GL for the OpenGL and Zink renderers, beside NVK,
# as tico-drastic links them; Zink reaches NVK through its ICD entry points.
LINK_FLAGS="$LINK_FLAGS -Wl,-u,vk_icdGetInstanceProcAddr -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion"
LINK_LIBS="$LINK_LIBS -l:libGL.a -l:libEGL.a -l:libglapi.a"
LINK_LIBS="$LINK_LIBS -l:libmesa_util_c11.a -l:libblake3.a -l:libmesa_util.a -l:libmesa_util_simd.a -l:libxmlconfig.a"
LINK_LIBS="$LINK_LIBS $NVK_DEPS"

# Mesa merges NVK's archives with the host ar, which leaves the Rust members
# out of the symbol index; rebuild it with the devkitA64 archiver.
NVK_ARCHIVE="$BUILD_DIR/libvulkan.a"
cp "$NVK_ARCHIVE_SRC" "$NVK_ARCHIVE"
"$DEVKITA64/bin/aarch64-none-elf-ranlib" "$NVK_ARCHIVE"

LINK_LIBS="$LINK_LIBS -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -larchive -lbz2 -llzma -llz4 -lz -lzstd"
LINK_LIBS="$LINK_LIBS -lusbhsfs -lusbntfs -lnx -lm -lstdc++ -lpthread"

$CXX $LINK_FLAGS \
    "${TICO_OBJS[@]}" \
    "${GLSLANG_LIBS[@]}" \
    -Wl,--start-group "${CORE_LIBS[@]}" "$NVK_ARCHIVE" $LINK_LIBS -Wl,--end-group \
    -o "$ELF_OUTPUT"

if [ $? -ne 0 ]; then
    echo "Error: Linking failed"
    exit 1
fi

echo "ELF created: $ELF_OUTPUT"

# ============================================================
# Step 4: Convert to NRO
# ============================================================
echo "--- Step 4: Creating NRO ---"

NRO_OUTPUT="$BUILD_DIR/tico-swanstation.nro"
ELF2NRO="$DEVKITPRO/tools/bin/elf2nro"
NACPTOOL="$DEVKITPRO/tools/bin/nacptool"

# Create NACP
NACP_FILE="$BUILD_DIR/swanstation.nacp"
$NACPTOOL --create "tico SwanStation" "ticoverse.com" "$APP_VERSION" "$NACP_FILE"

# Convert ELF to NRO with romfs
ROMFS_DIR="$BUILD_DIR/romfs"
rm -rf "$ROMFS_DIR"
mkdir -p "$ROMFS_DIR"

[ -d "$TICO_DIR/fonts" ] && cp -r "$TICO_DIR/fonts" "$ROMFS_DIR/"
[ -d "$TICO_DIR/lang" ] && cp -r "$TICO_DIR/lang" "$ROMFS_DIR/"
[ -d "$TICO_DIR/assets" ] && cp -r "$TICO_DIR/assets" "$ROMFS_DIR/"
[ -d "$TICO_DIR/shaders" ] && cp -r "$TICO_DIR/shaders" "$ROMFS_DIR/"
# the overlay builds its settings menu from the module's own definition
mkdir -p "$ROMFS_DIR/module"
cp "$TICO_DIR/module/settings.json" "$ROMFS_DIR/module/"

ELF2NRO_ARGS=(--nacp="$NACP_FILE")

if [ -n "$(find "$ROMFS_DIR" -type f 2>/dev/null | head -1)" ]; then
    echo "Embedding romfs from: $ROMFS_DIR"
    ELF2NRO_ARGS+=(--romfsdir="$ROMFS_DIR")
fi

$ELF2NRO "$ELF_OUTPUT" "$NRO_OUTPUT" "${ELF2NRO_ARGS[@]}"

if [ ! -f "$NRO_OUTPUT" ]; then
    echo "Error: tico-swanstation.nro not found"
    exit 1
fi

#---------------------------------------------------------------------------------
# Module bundle
#
# A module is a directory, not a bare NRO: tico discovers it by reading
# module.json, and everything the module owns -- its settings definition,
# gamelist and console artwork -- travels with it. Shipping only the NRO would
# mean a new system or a changed option tree still needs a tico release.
#
# The NRO sits beside module.json, so an installed bundle is self-contained and
# extracts straight into sdmc:/tico/modules/<id>/.
#---------------------------------------------------------------------------------
MODULE_SRC="$TICO_DIR/module"
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$BUILD_DIR/module/$MODULE_ID"

rm -rf "$BUILD_DIR/module"
mkdir -p "$MODULE_OUT"
# The zip is the complete module. romfs is per-NRO, so tico cannot read anything
# out of this core's romfs -- everything tico needs about the module has to reach
# the SD card, and the bundle is what carries it. Tico's own romfs holds a copy of
# the official modules only as an offline baseline for a fresh install.
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp "$NRO_OUTPUT" "$MODULE_OUT/"
# tico merges these into its own strings to label the settings screen
cp -R "$TICO_DIR/lang" "$MODULE_OUT/"

# Tico prefers .json.gz when resolving a gamelist.
if [ -d "$MODULE_OUT/gamelists" ]; then
    gzip -f -9 "$MODULE_OUT"/gamelists/*.json 2>/dev/null || true
fi

BUNDLE="$BUILD_DIR/tico-$MODULE_ID-module.zip"
rm -f "$BUNDLE"
( cd "$BUILD_DIR/module" && zip -qr "$BUNDLE" "$MODULE_ID" )

echo "======================================"
echo "Build successful!"
echo "  NRO:    $NRO_OUTPUT"
echo "  Module: $BUNDLE"
echo "          extracts to sdmc:/tico/modules/$MODULE_ID/"
echo "======================================"
find "$MODULE_OUT" -type f | sed "s|$BUILD_DIR/module/|    |"
