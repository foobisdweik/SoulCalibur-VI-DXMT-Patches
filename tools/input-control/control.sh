#!/bin/zsh
# Send one command to an already running game. This does not launch SC6.
set -euo pipefail
here=${0:A:h}
client=$here/../../dll/build/input-control.exe
[[ -f $client ]] || "$here/build.sh"
wine=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver/bin/wine
args=("$@")
if [[ ${args[1]:-} == stop && ${args[2]:-} == /* ]]; then
  args[2]="Z:${args[2]}"
fi
exec "$wine" --bottle "${SC6_BOTTLE:-SC6-FEX}" "$client" "${args[@]}"
