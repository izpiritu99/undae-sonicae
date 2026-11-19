#!/bin/sh
# macOS build of the native edition: one universal app (Apple Silicon + Intel), self-tested, signed ad hoc, and packed
# into a .dmg. Needs the Xcode command line tools (xcode-select --install), CMake and Ninja (brew install cmake ninja),
# and JUCE in cpp/third_party/JUCE (see HANDOVER.md, "Native edition on macOS").
#   cpp/build-mac.sh [Release|Debug]
set -e
cd "$(dirname "$0")/.."
CONFIG="${1:-Release}"
for t in cmake ninja xcrun; do command -v "$t" >/dev/null || { echo "missing: $t (see the comment at the top of this script)"; exit 1; }; done
[ -f cpp/third_party/JUCE/CMakeLists.txt ] || { echo "JUCE is missing: unzip JUCE-8.0.15-patched.zip into cpp/third_party"; exit 1; }

cmake -S cpp -B cpp/build-mac -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build cpp/build-mac

B=cpp/build-mac
APP="$B/UndaeSonicae_artefacts/$CONFIG/Undae Sonicae.app"
"$B/undae-selftest_artefacts/$CONFIG/undae-selftest" cpp/examples/first-light.undae.json | tail -n 1

# Ad hoc signature: enough for this Mac and, after "Open" from the right-click menu once, for others. Distributing to
# other people without that step needs a Developer ID signature and notarisation (HANDOVER.md).
codesign --force --deep --options runtime --sign - "$APP"
codesign --verify --deep --strict "$APP" && echo "signed (ad hoc): $APP"

mkdir -p cpp/dist
DMG="cpp/dist/Undae-Sonicae-0.3.0-mac.dmg"
STAGE="$(mktemp -d)"; cp -R "$APP" "$STAGE/"; ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"; hdiutil create -quiet -volname "Undae Sonicae" -srcfolder "$STAGE" -format UDZO "$DMG"; rm -rf "$STAGE"
echo "packed $DMG (open it and drag Undae Sonicae to Applications)"
