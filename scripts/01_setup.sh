#!/bin/bash
set -e

echo "=== 0. Installing Dependencies ==="
if [ -f /etc/os-release ]; then
    . /etc/os-release
    if [[ "$ID" == "fedora" || "$ID_LIKE" == *"fedora"* || "$ID" == "rhel" || "$ID" == "centos" ]]; then
        echo "Detected Fedora/RHEL-based OS. Installing dependencies with dnf..."
        sudo dnf install -y cmake gcc-c++ make python3 python3-devel scons mold patch zlib-devel m4 ccache gperftools-devel libpng-devel hdf5-devel capstone-devel protobuf-compiler protobuf-devel gettext libX11-devel xorg-x11-server-devel libXt-devel libXmu-devel libXi-devel
    elif [[ "$ID" == "ubuntu" || "$ID" == "debian" || "$ID_LIKE" == *"debian"* ]]; then
        echo "Detected Ubuntu/Debian-based OS. Installing dependencies with apt-get..."
        sudo apt-get update
        sudo apt-get install -y cmake g++ make python3 python3-dev scons mold patch zlib1g-dev m4 ccache libgoogle-perftools-dev libpng-dev libhdf5-dev libcapstone-dev protobuf-compiler libprotobuf-dev gettext libx11-dev xorg-dev libxt-dev libxmu-dev libxi-dev
    else
        echo "Unsupported OS: $ID. Please install cmake, gcc, make, python3, scons, and mold manually."
    fi
else
    echo "Could not detect OS. Please install dependencies manually."
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ROOT_DIR="$(realpath "$SCRIPT_DIR/..")"
EXT_DIR="$ROOT_DIR/ext"

cd "$ROOT_DIR"

echo "=== 1. Initializing submodules ==="
# Pull the submodules listed in .gitmodules
git submodule update --init --recursive

echo "=== 2. Applying Patches ==="
./scripts/00_patch_manager.sh apply

echo "=== 3. Building DynamoRIO ==="
cd "$EXT_DIR/dynamorio"
mkdir -p build && cd build
cmake ..
make -j$(nproc)

echo "=== 4. Building Custom DR Tool ==="
cd "$ROOT_DIR/tools/trace_analyzer"
mkdir -p build && cd build
cmake ..
make -j$(nproc)

echo "=== 5. Building GAPBS ==="
cd "$EXT_DIR/gapbs"
make -j$(nproc)

echo "=== 6. Building gem5 (Fast) ==="
cd "$EXT_DIR/gem5"
scons build/X86/gem5.opt -j$(nproc) --linker=mold --ignore-style USE_CCACHE=1

echo "=== 7. Fetching PARSEC Inputs ==="
cd "$EXT_DIR/parsec-benchmark"
./get-inputs

echo "=== 8. Building PARSEC ==="
cd "$EXT_DIR/parsec-benchmark"
./bin/parsecmgmt -a build -p blackscholes bodytrack canneal dedup facesim ferret fluidanimate freqmine streamcluster swaptions vips x264 -c gcc

echo "=== Setup Complete! ==="
