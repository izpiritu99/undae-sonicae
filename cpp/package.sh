#!/bin/sh
# Package the native edition for Linux: a .deb and an AppImage in dist/ (next to the Electron ones).
#   sh cpp/package.sh
# The app goes to /opt/undae-sonicae-native with the plug-in hosts beside it (the app finds them there); `undae-sonicae-native`
# starts it. The AppImage is the same folder with the standard AppImage runtime in front (taken from electron-builder's cache).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=0.3.0
B="$ROOT/cpp/build"
cmake -S "$ROOT/cpp" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$B" --target UndaeSonicae
sh "$ROOT/native/build.sh" > /dev/null 2>&1 || true   # the plug-in hosts (skipped quietly when a compiler for them is missing)
STAGE="$B/pkg"; rm -rf "$STAGE"; mkdir -p "$ROOT/dist"

# the app folder, used by both packages
app() {
  D="$1"; mkdir -p "$D"
  cp "$B/UndaeSonicae_artefacts/Release/Undae Sonicae" "$D/undae-sonicae"
  for f in undae-host undae-test.clap undae-vsthost.exe undae-vsthost32.exe; do [ -f "$ROOT/native/bin/$f" ] && cp "$ROOT/native/bin/$f" "$D/"; done
  cp "$ROOT/app/img/wordmark.png" "$D/wordmark.png"
  cp "$ROOT/app/icons/icon-512.png" "$D/icon.png"
  strip "$D/undae-sonicae" 2> /dev/null || true
}
desktop() {   # $1: Exec line
  cat << EOF
[Desktop Entry]
Type=Application
Name=Undae Sonicae (native)
GenericName=Music production
Comment=Pattern-based music studio
Exec=$1 %f
Icon=undae-sonicae-native
Terminal=false
Categories=AudioVideo;Audio;Sequencer;Midi;
MimeType=application/json;
EOF
}

# --- .deb ---
R="$STAGE/deb"; app "$R/opt/undae-sonicae-native"
mkdir -p "$R/usr/bin" "$R/usr/share/applications" "$R/usr/share/icons/hicolor/512x512/apps" "$R/usr/share/icons/hicolor/64x64/apps" "$R/DEBIAN"
ln -s /opt/undae-sonicae-native/undae-sonicae "$R/usr/bin/undae-sonicae-native"
desktop undae-sonicae-native > "$R/usr/share/applications/undae-sonicae-native.desktop"
cp "$ROOT/app/icons/icon-512.png" "$R/usr/share/icons/hicolor/512x512/apps/undae-sonicae-native.png"
cp "$ROOT/app/icons/icon-64.png" "$R/usr/share/icons/hicolor/64x64/apps/undae-sonicae-native.png"
SIZE=$(du -sk "$R/opt" | cut -f1)
cat > "$R/DEBIAN/control" << EOF
Package: undae-sonicae-native
Version: $VER
Section: sound
Priority: optional
Architecture: amd64
Installed-Size: $SIZE
Depends: libasound2t64 | libasound2, libfreetype6, libfontconfig1, libpng16-16t64 | libpng16-16, libx11-6, libxext6, libxinerama1, libxrandr2, libxcursor1
Recommends: pipewire-jack | libjack-jackd2-0, libmp3lame0
Suggests: wine
Maintainer: Undae Sonicae <dev@undae-sonicae.invalid>
Description: Undae Sonicae, native edition
 Pattern-based music studio: channel rack, piano roll, playlist, mixer, synth,
 sampler, audio lab and sound design, with CLAP / VST3 plug-ins and Windows VSTs
 through Wine. Native C++ (JUCE), no browser engine.
EOF
dpkg-deb --build --root-owner-group "$R" "$ROOT/dist/undae-sonicae-native_${VER}_amd64.deb" > /dev/null
echo "built dist/undae-sonicae-native_${VER}_amd64.deb"

# --- AppImage ---
# runtime: the current static one if it is in ~/.cache/undae-sonicae (github.com/AppImage/type2-runtime, needs no libfuse2),
# else electron-builder's older one (needs libfuse2 at run time)
AI="$HOME/.cache/electron-builder/appimage/appimage-12.0.1"
RT="$HOME/.cache/undae-sonicae/runtime-x86_64"; [ -f "$RT" ] || RT="$AI/runtime-x64"
if [ -x "$AI/linux-x64/mksquashfs" ] && [ -f "$RT" ]; then
  A="$STAGE/AppDir"; app "$A/usr/lib/undae-sonicae"
  cat > "$A/AppRun" << 'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/lib/undae-sonicae/undae-sonicae" "$@"
EOF
  chmod +x "$A/AppRun"
  desktop undae-sonicae > "$A/undae-sonicae-native.desktop"
  cp "$ROOT/app/icons/icon-512.png" "$A/undae-sonicae-native.png"; ln -sf undae-sonicae-native.png "$A/.DirIcon"
  OUT="$ROOT/dist/Undae Sonicae Native-$VER.AppImage"
  "$AI/linux-x64/mksquashfs" "$A" "$STAGE/app.squashfs" -root-owned -noappend -comp gzip -quiet > /dev/null
  cat "$RT" "$STAGE/app.squashfs" > "$OUT"; chmod +x "$OUT"
  echo "built dist/Undae Sonicae Native-$VER.AppImage"
else
  echo "AppImage skipped: no AppImage runtime found (run the Electron packaging once, or install appimagetool)"
fi
