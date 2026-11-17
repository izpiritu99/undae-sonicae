#!/bin/sh
# Give the Electron AppImage the current static AppImage runtime (no libfuse2 needed at run time). electron-builder writes
# one with the older runtime; this keeps its contents and swaps the launcher in front. Skipped when the runtime is not in
# ~/.cache/undae-sonicae/runtime-x86_64 (from github.com/AppImage/type2-runtime).
RT="$HOME/.cache/undae-sonicae/runtime-x86_64"
[ -f "$RT" ] || { echo "appimage-runtime: no static runtime in $RT, AppImage left as built"; exit 0; }
for f in dist/*.AppImage; do
  case "$f" in *Native*) continue;; esac   # the native one is built with it already
  OFF=$("./$f" --appimage-offset 2>/dev/null) || continue
  [ "$(head -c "$(stat -c %s "$RT")" "$f" | cmp -s - "$RT"; echo $?)" = 0 ] && continue   # already swapped
  cat "$RT" > "$f.new" && tail -c +$((OFF + 1)) "$f" >> "$f.new" && chmod +x "$f.new" && mv "$f.new" "$f" && echo "appimage-runtime: $f now needs no libfuse2"
done
