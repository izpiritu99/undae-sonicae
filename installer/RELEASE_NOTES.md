Undae Sonicae, native edition: a pattern-based DAW with a step-sequencer rack, piano roll, song arranger and mixer, its
own synth and sampler, 40+ built-in effects, and plug-ins on the desktop. Projects (`.undae.json`) open on every platform.

## Downloads

| Platform | File | Notes |
| --- | --- | --- |
| Windows 10 / 11 (64-bit) | `Undae-Sonicae-*-windows-setup.exe` | Installer: Start menu entry, uninstaller, VST2 / VST3 plug-ins (64- and 32-bit), MP3 export. Windows may show "Windows protected your PC" (the installer is not code-signed): click **More info › Run anyway**. |
| Windows, no install | `Undae-Sonicae-*-windows-portable.zip` | Unzip anywhere and run `Undae Sonicae.exe`. |
| macOS 11+ (Intel and Apple Silicon) | `Undae-Sonicae-*-mac.dmg` | Open it and drag Undae Sonicae to Applications. The first time, macOS blocks it (not notarised): open **System Settings › Privacy & Security** and click **Open Anyway**. Audio Units, VST3 and VST2. |
| Linux (x86-64) | `Undae-Sonicae-*-linux-x86_64.AppImage` | `chmod +x` it and run it. |
| Ubuntu / Debian | `undae-sonicae_*_amd64.deb` | `sudo apt install ./undae-sonicae_*_amd64.deb`; start "Undae Sonicae (native)" from the menu. CLAP and VST3 plug-ins; Windows VSTs through Wine. |
| Android 8+ (arm64) | `Undae-Sonicae-*-android.apk` | Allow installing from this source when asked. If an older copy signed by another computer is installed, remove it first. Plug-ins come from a computer over Wi-Fi (Settings › Plug-in server). |

Plug-in folders: Windows uses the standard VST folders (add more with the `UNDAE_VST_PATH` environment variable,
separated by `;`); macOS uses `/Library/Audio/Plug-Ins` and `~/Library/Audio/Plug-Ins`.
