#!/bin/sh
# Installs build/US1800.driver into the system HAL plug-in folder and restarts coreaudiod. Run with sudo.
set -e
cd "$(dirname "$0")/.."
rm -rf /Library/Audio/Plug-Ins/HAL/US1800.driver
cp -R build/US1800.driver /Library/Audio/Plug-Ins/HAL/
chown -R root:wheel /Library/Audio/Plug-Ins/HAL/US1800.driver
killall coreaudiod || true
echo "installed; coreaudiod restarting"
