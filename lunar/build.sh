#!/bin/sh
set -e

echo "=== okpm v3.0.0 build ==="

if ! command -v gcc >/dev/null 2>&1; then
    echo "Error: gcc not found. Please install gcc first."
    echo "  Debian/Ubuntu: sudo apt install gcc"
    echo "  Arch:          sudo pacman -S gcc"
    echo "  Fedora:        sudo dnf install gcc"
    exit 1
fi

if command -v make >/dev/null 2>&1; then
    make
else
    gcc -Wall -Wextra -O2 -D_GNU_SOURCE -o okpm okpm.c
fi

if [ -f okpm ]; then
    echo ""
    echo "Build successful!"
    echo ""
    echo "Quick test:"
    ./okpm version
    ./okpm help | head -5
    echo ""
    echo "Install to system:"
    echo "  sudo make install"
    echo ""
    echo "Try the example package:"
    echo "  chmod +x template/scripts/* template/files/usr/bin/hello"
    echo "  ./okpm build template/"
    echo "  sudo ./okpm install hello-1.0.0.okra"
    echo "  hello"
else
    echo "Build failed!"
    exit 1
fi