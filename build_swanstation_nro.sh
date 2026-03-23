#!/bin/bash

export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITPPC=$DEVKITPRO/devkitPPC
export DEVKITA64=$DEVKITPRO/devkitA64

echo "=== Building Swanstation NRO with Tico Overlay ==="

# Clean build directory
rm -rf build
mkdir -p build
cd build

# Configure CMake to build the NRO
# Note: we pass -DUSE_TICO=ON and -DPLATFORM=libnx
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DPLATFORM=libnx \
    -DBUILD_LIBRETRO_CORE=ON \
    -DLIBRETRO_STATIC=ON \
    -DENABLE_MMAP_FASTMEM=ON \
    -DENABLE_VULKAN=OFF \
    -DENABLE_OPENGL=ON \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DENABLE_LTO=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DDISABLE_LOGGING=1 \
    -DCMAKE_C_FLAGS="-DDISABLE_LOGGING=1 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -Os -ffunction-sections -fdata-sections -D__SWITCH__ -fno-lto -fvisibility=hidden -fomit-frame-pointer -fno-ident -fstack-protector-strong" \
    -DCMAKE_CXX_FLAGS="-DDISABLE_LOGGING=1 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -Os -ffunction-sections -fdata-sections -D__SWITCH__ -fno-lto -fvisibility=hidden -fvisibility-inlines-hidden -fomit-frame-pointer -fno-ident -fstack-protector-strong" \
    -DUSE_TICO=ON

# Build all targets to ensure NRO is generated
make -j$(nproc)

if [ -f "swanstation_tico.nro" ]; then
    echo "======================================"
    echo "Build successful!"
    echo "======================================"
else
    echo "Error: swanstation_tico.nro not found"
    exit 1
fi