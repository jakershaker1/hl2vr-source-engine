#!/bin/sh
# Cross-compile for arm64-v8a (Quest / Pico / Galaxy XR are all 64-bit only).
# Uses whatever NDK is pointed to by ANDROID_NDK_HOME (tested with NDK r27).

set -e

git submodule init && git submodule update

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your Android NDK install}"

./waf configure -T debug --android=aarch64,clang,28 --disable-warns &&
./waf build
