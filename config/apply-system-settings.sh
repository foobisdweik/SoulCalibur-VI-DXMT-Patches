#!/bin/zsh
# Merge the [SystemSettings] section of config/Engine.SystemSettings.ini into the
# game's user Engine.ini in the SC6-FEX bottle. The other sections stay as they
# are. The first run keeps a copy as Engine.ini.before-sc6vrr-settings.
# Undo: delete the [SystemSettings] section from Engine.ini.
set -euo pipefail
here=${0:A:h}
bottle=${SC6_BOTTLE:-SC6-FEX}
ini="$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/users/crossover/AppData/Local/SoulcaliburVI/Saved/Config/WindowsNoEditor/Engine.ini"
pgrep -f -q '[S]oulcaliburVI.exe' && { print -u2 "Quit SoulCalibur VI first: it rewrites Engine.ini when it exits."; exit 1; }
[[ -f $ini ]] || { print -u2 "Missing: $ini"; exit 1; }
[[ -f $ini.before-sc6vrr-settings ]] || cp -p "$ini" "$ini.before-sc6vrr-settings"
/usr/bin/python3 -I - "$here/Engine.SystemSettings.ini" "$ini" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
wanted = [l.strip() for l in open(src) if re.match(r"\s*[\w.]+\s*=", l)]
text = open(dst, newline="").read()
nl = "\r\n" if "\r\n" in text else "\n"
lines = text.splitlines()
# drop any earlier [SystemSettings] section
out, skip = [], False
for l in lines:
    if l.strip().startswith("["):
        skip = l.strip().lower() == "[systemsettings]"
    if not skip:
        out.append(l)
while out and not out[-1].strip():
    out.pop()
out += ["", "[SystemSettings]"] + wanted + [""]
open(dst, "w", newline="").write(nl.join(out) + nl)
print(f"Merged {len(wanted)} settings into {dst}")
PY
