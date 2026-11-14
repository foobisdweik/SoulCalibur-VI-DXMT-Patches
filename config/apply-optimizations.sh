#!/bin/zsh
# Apply the parts of sc6-optimizations.ini (next to SoulcaliburVI.exe) that
# d3d11.dll cannot apply itself. d3d11.dll reads every other toggle, and
# follows changes to the file while the game runs
# (src/d3d11/d3d11_sc6_toggles.hpp in dxmt-SC6).
#
#   PostProcessingOff = true    merge config/Engine.SystemSettings.ini into the
#                               game's Engine.ini (apply-system-settings.sh)
#   PostProcessingOff = false   remove that [SystemSettings] section again
#   WatchPostProcessingOff      log each run, and the section's settings, to
#                               $data/logs/sc6-optimizations-watch.log
#
# A name the file leaves out counts as true for PostProcessingOff and false for
# its Watch line. The game rewrites Engine.ini when it exits, so nothing
# changes while it runs; dll/apply-ini.sh runs this script, and the
# local.sc6vrr.apply-ini LaunchAgent runs that when either ini changes.
set -euo pipefail
here=${0:A:h}
bottle=${SC6_BOTTLE:-SC6-FEX}
win64=${SC6_WIN64:-$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64}
toggles=$win64/sc6-optimizations.ini
engine="$HOME/Library/Application Support/CrossOver/Bottles/$bottle/drive_c/users/crossover/AppData/Local/SoulcaliburVI/Saved/Config/WindowsNoEditor/Engine.ini"
log="$HOME/Library/Application Support/SC6-VRR/logs/sc6-optimizations-watch.log"
[[ -f $toggles ]] || exit 0

value() {  # value <name> <default>: true or false
  local v
  v=$(sed -n -E "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*([A-Za-z01]+).*/\1/p" "$toggles" | tail -1 | tr A-Z a-z)
  case $v in true|1|on|yes) print true ;; false|0|off|no) print false ;; *) print $2 ;; esac
}
off=$(value PostProcessingOff true)
watch=$(value WatchPostProcessingOff false)
note() {
  print -r -- "$1"
  if [[ $watch == true ]]; then
    mkdir -p "${log:h}"
    print -r -- "$(date '+%F %T') [watch] PostProcessingOff: $1" >> "$log"
  fi
}

if pgrep -f -q '[S]oulcaliburVI.exe'; then
  note "the game is running, so Engine.ini stays as it is until the next sync"
  exit 0
fi
if [[ $off == true ]]; then
  note "$("$here/apply-system-settings.sh")"
else
  note "$("$here/apply-system-settings.sh" --remove)"
fi
if [[ $watch == true ]]; then
  settings=$(awk '/^\[/{p = tolower($0) == "[systemsettings]"; next} p && /=/' "$engine" | tr -d '\r' | paste -sd ' ' -)
  note "Engine.ini [SystemSettings]: ${settings:-none}"
fi
