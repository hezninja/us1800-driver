#!/bin/sh
# Builds build/US1800.driver (ad-hoc signed). Install with plugin/install.sh.
set -e
cd "$(dirname "$0")/.."
B=build/US1800.driver
rm -rf "$B"
mkdir -p "$B/Contents/MacOS"
cp plugin/Info.plist "$B/Contents/Info.plist"
clang -O2 -Wall -Wextra -arch arm64 -bundle -o "$B/Contents/MacOS/US1800" plugin/us1800_plugin.c \
    -framework CoreAudio -framework CoreFoundation
codesign --force --sign - "$B"
echo "built $B"
