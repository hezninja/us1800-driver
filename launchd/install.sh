#!/bin/sh
# Installs the login item: the daemon starts at login and restarts if it exits.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
mkdir -p ~/Library/LaunchAgents ~/Library/Logs
cp "$D/local.us1800d.plist" ~/Library/LaunchAgents/
pkill us1800d || true
sleep 2
launchctl bootout gui/$(id -u)/local.us1800d 2>/dev/null || true
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.us1800d.plist
echo "installed; remove with: launchctl bootout gui/$(id -u)/local.us1800d && rm ~/Library/LaunchAgents/local.us1800d.plist"
