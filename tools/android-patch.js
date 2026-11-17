// Run once after `cap add android`: adds the microphone permission and declares optional MIDI / USB-host support.
const fs = require('fs'), path = require('path');
const file = path.join(__dirname, '..', 'android', 'app', 'src', 'main', 'AndroidManifest.xml');
if (!fs.existsSync(file)) { console.error('android/ not found - run `npx cap add android` first'); process.exit(1); }
let xml = fs.readFileSync(file, 'utf8');
const lines = [
  '<uses-permission android:name="android.permission.RECORD_AUDIO" />',
  '<uses-permission android:name="android.permission.MODIFY_AUDIO_SETTINGS" />',
  '<uses-feature android:name="android.hardware.microphone" android:required="false" />',
  '<uses-feature android:name="android.software.midi" android:required="false" />',
  '<uses-feature android:name="android.hardware.usb.host" android:required="false" />',
].filter(l => !xml.includes(l.match(/android:name="([^"]+)"/)[1]));
if (lines.length) { xml = xml.replace('</manifest>', '    ' + lines.join('\n    ') + '\n</manifest>'); fs.writeFileSync(file, xml); }
console.log(lines.length ? `Patched AndroidManifest.xml (+${lines.length} entries)` : 'AndroidManifest.xml already patched');
