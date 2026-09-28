#!/bin/sh
# reclaim-disk.sh - best-effort free-space reclaim on the macOS runner before
# an M150 iOS build (same idea as build-ios.yml's inline step, lifted out so
# build-m150-ios.yml can call one line). Never fails the job: every command
# is allowed to fail, and the script itself always exits 0.
set +e
xcrun simctl delete all >/dev/null 2>&1
sudo rm -rf "$HOME/Library/Developer/CoreSimulator/Caches"/* >/dev/null 2>&1
sudo rm -rf /usr/local/lib/android "${ANDROID_HOME:-}" >/dev/null 2>&1
sudo rm -rf /Applications/Xcode_16*.app /Applications/Xcode_15*.app >/dev/null 2>&1
df -h /
exit 0
