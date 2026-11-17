# Builds the Windows VST hosts (native\bin\undae-vsthost.exe and undae-vsthost32.exe) on Windows, the way build.sh
# does on Linux: MinGW-w64 gcc compiles vsthost.c freestanding (no C library of its own) and ld links it to the system
# DLLs, through import libraries made here with dlltool from win\imports.def.txt.
# Needs gcc, ld, objcopy and dlltool from a MinGW-w64 with the i386pep and i386pe ld emulations (WinLibs has them):
#   powershell -ExecutionPolicy Bypass -File native\build-win.ps1 [-MinGW <its bin folder>]
param([string]$MinGW = "")
$ErrorActionPreference = "Stop"
if ($MinGW) { $env:Path = "$MinGW;$env:Path" }
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force bin, bin\imp | Out-Null

# Two flags the Linux build does not need: -mno-stack-arg-probe (MinGW's gcc would call __chkstk_ms from libgcc for big
# stack frames) and --disable-runtime-pseudo-reloc (MinGW's ld would want the C runtime's relocator).
$cflags = "-c -O2 -Wall -Wextra -fno-merge-constants -ffreestanding -fno-stack-protector -fno-pic -fno-pie " +
          "-fno-asynchronous-unwind-tables -fcf-protection=none -fno-ident -mno-stack-arg-probe" -split " "

# win\imports.def.txt -> one .def per DLL
$defs = @{}; $cur = $null
foreach ($l in Get-Content win\imports.def.txt) {
    if ($l -match '^\s*#' -or $l -match '^\s*$') { continue }
    if ($l -match '^\[(.+)\]$') { $cur = $Matches[1]; $defs[$cur] = @(); continue }
    $defs[$cur] += ($l.Trim() -split '\s+')
}

function Run($exe, [string[]]$a) { & $exe @a; if ($LASTEXITCODE) { throw "$exe failed ($LASTEXITCODE)" } }

function Build($bits) {
    if ($bits -eq 64) {
        Run gcc ($cflags + @("-mabi=ms", "-mno-red-zone", "-o", "bin\vsthost.o", "win\vsthost.c"))
        $emu = "i386pep"; $entry = "entry"; $out = "bin\undae-vsthost.exe"; $machine = "i386:x86-64"
    } else {
        Run gcc ($cflags + @("-m32", "-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2", "-malign-double", "-fleading-underscore", "-o", "bin\vsthost.o", "win\vsthost.c"))
        $emu = "i386pe"; $entry = "_entry"; $out = "bin\undae-vsthost32.exe"; $machine = "i386"
    }
    # 32-bit stdcall imports are decorated (_Sleep@4). Take the exact names the object wants from nm; each .def line
    # "Sleep@4 == Sleep" gives the link symbol (dlltool -U adds the _) and, after ==, the plain name the DLL exports.
    $want = @{}
    if ($bits -eq 32) {
        foreach ($l in (& nm -u bin\vsthost.o)) { if ($l -match 'U _(\w+)@(\d+)$') { $want[$Matches[1]] = "$($Matches[1])@$($Matches[2])" } }
    }
    $libs = foreach ($dll in $defs.Keys) {
        $base = [IO.Path]::GetFileNameWithoutExtension($dll)
        $def = "bin\imp\$base-$bits.def"; $lib = "bin\imp\lib$base-$bits.a"
        $names = foreach ($n in $defs[$dll]) { if ($bits -eq 64) { $n } elseif ($want.ContainsKey($n)) { "$($want[$n]) == $n" } else { "$n == $n" } }
        (@("LIBRARY $dll", "EXPORTS") + $names) | Set-Content -Encoding ascii $def
        $extra = if ($bits -eq 64) { @("--no-leading-underscore") } else { @("-U", "--as-flags=--32") }   # dlltool's own `as` is 64-bit by default
        Run dlltool (@("-m", $machine) + $extra + @("-d", $def, "-l", $lib)); $lib
    }
    Run objcopy @("-R", ".comment", "-R", ".note.GNU-stack", "-R", ".note.gnu.property", "-R", ".eh_frame", "bin\vsthost.o")
    Run ld (@("-m", $emu, "--subsystem", "console", "--image-base", "0x400000", "--disable-dynamicbase", "--enable-stdcall-fixup",
              "--disable-auto-import", "--disable-runtime-pseudo-reloc", "-e", $entry, "-o", $out, "bin\vsthost.o") + $libs)
    Remove-Item bin\vsthost.o
    Write-Host "built native\$out ($bits-bit Windows plug-ins)"
}
Build 64
Build 32
Remove-Item -Recurse bin\imp
