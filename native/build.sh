#!/bin/sh
# Builds the plug-in host and the test plug-in into native/bin/. Needs gcc (sudo apt install build-essential).
set -e
cd "$(dirname "$0")"
mkdir -p bin
CFLAGS="-O2 -g -Wall -Wextra -Iinclude"
gcc $CFLAGS -pthread -o bin/undae-host host.c -ldl -lm -l:libX11.so.6
gcc $CFLAGS -fPIC -shared -o bin/undae-test.clap testplug.c -lm -l:libX11.so.6
gcc $CFLAGS -o bin/xpoke xpoke.c -l:libX11.so.6

# The Windows VST host, for Wine. Built with the Linux compiler: Windows calling convention (-mabi=ms), no C library of
# its own, linked straight against the DLLs Wine ships. Skipped when Wine is not installed.
WINE=/usr/lib/x86_64-linux-gnu/wine
WIN_CFLAGS="-c -O2 -Wall -Wextra -fno-merge-constants -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fcf-protection=none -fno-ident"
pe_link() {   # pe_link <ld emulation> <entry symbol> <dll folder> <output>  (the object is bin/vsthost.o)
  objcopy -R .comment -R .note.GNU-stack -R .note.gnu.property -R .eh_frame bin/vsthost.o
  ld -m "$1" --subsystem console --image-base 0x400000 --disable-dynamicbase --enable-stdcall-fixup -e "$2" -o "$4" bin/vsthost.o \
    "$3/msvcrt.dll" "$3/kernel32.dll" "$3/ws2_32.dll" "$3/advapi32.dll" "$3/user32.dll"
  rm bin/vsthost.o
}
if [ -f "$WINE/x86_64-windows/kernel32.dll" ]; then
  gcc $WIN_CFLAGS -mabi=ms -mno-red-zone -o bin/vsthost.o win/vsthost.c
  pe_link i386pep entry "$WINE/x86_64-windows" bin/undae-vsthost.exe
  echo "built native/bin/undae-vsthost.exe (64-bit Windows plug-ins)"
  if [ -f "$WINE/i386-windows/kernel32.dll" ]; then
    gcc $WIN_CFLAGS -m32 -msse2 -mfpmath=sse -mincoming-stack-boundary=2 -malign-double -fleading-underscore -o bin/vsthost.o win/vsthost.c
    pe_link i386pe _entry "$WINE/i386-windows" bin/undae-vsthost32.exe
    echo "built native/bin/undae-vsthost32.exe (32-bit Windows plug-ins)"
  fi
else
  echo "Wine not found: skipping the Windows VST host"
fi
echo "built native/bin/undae-host and native/bin/undae-test.clap"
