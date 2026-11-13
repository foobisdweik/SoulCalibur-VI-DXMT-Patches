#!/bin/zsh
# Install a staged aarch64 DXMT build into CrossOver Preview, for A/B tests.
#
# A build folder holds aarch64-windows/{d3d10core,d3d11,dxgi,nvapi64,nvngx,
# winemetal}.dll and aarch64-unix/winemetal.so. All 7 files are swapped
# together, because winemetal.dll and winemetal.so share one table of unix
# calls, and a mismatched pair crashes at device creation.
#
# Usage:
#   dxmt-swap.sh list            staged builds; "*" marks the installed one
#   dxmt-swap.sh status          the installed build's version
#   dxmt-swap.sh <name|folder>   install a build; names are listed below
#
# Names: fork (v0.80-407, the SC6 fork without Mullion), mullion (the current
# Mullion-based SC6 build), stock (Preview 41303's own aarch64 DXMT), or any
# folder name under builds/ or backups/ in the SC6-VRR data folder.
#
# Before it replaces files, the script copies the installed set to backups/
# unless that set is already staged somewhere (same d3d11.dll checksum).
# It refuses to run while the game runs, because Wine keeps the DLLs mapped.
# The shader caches stay separate per build: the fork uses
# $DARWIN_USER_CACHE_DIR/dxmt/, Mullion uses .../mullion/.
set -euo pipefail
data="$HOME/Library/Application Support/SC6-VRR"
cx=${CROSSOVER_APP:-/Applications/CrossOver Preview.app}/Contents/SharedSupport/CrossOver/lib/dxmt
files=(aarch64-windows/{d3d10core,d3d11,dxgi,nvapi64,nvngx,winemetal}.dll aarch64-unix/winemetal.so)
typeset -A alias=(
  fork    "$data/backups/crossover-preview-dxmt-aarch64-20261110-170444"
  mullion "$data/builds/mullion-best-20261113"
  stock   "$HOME/Ops/backups/crossover-preview-41303-dxmt-aarch64-20261113-013125"
)

version() {  # the build's version string, such as v0.80-1316-g41fe9c0b
  local v; v=$(strings "$1/aarch64-windows/d3d11.dll" 2>/dev/null | grep -E '^v[0-9]+\.[0-9]+-[0-9]+-g[0-9a-f]+' | head -1) || true
  print -r -- "${v:-unknown}"
}
sum() { shasum "$1/aarch64-windows/d3d11.dll" 2>/dev/null | cut -c1-40; }
complete() { local f; for f in $files; do [[ -f $1/$f ]] || return 1; done; }
same() { local f; for f in $files; do cmp -s "$1/$f" "$2/$f" || return 1; done; }
is_staged() { local d; for d in ${(f)"$(staged)"}; do [[ $(sum "$d") == $1 ]] && return 0; done; return 1; }
staged() { print -rl -- ${(v)alias} "$data"/builds/*(N/) "$data"/backups/*(N/) | sort -u | while read -r d; do complete "$d" && print -r -- "$d"; done; }

resolve() {
  local d=${alias[$1]:-}
  [[ -z $d && -d $1 ]] && d=${1:A}
  [[ -z $d && -d $data/builds/$1 ]] && d=$data/builds/$1
  [[ -z $d && -d $data/backups/$1 ]] && d=$data/backups/$1
  [[ -n $d ]] && complete "$d" || { print -u2 "No complete build for '$1'. Run: ${0:t} list"; exit 1; }
  print -r -- "$d"
}

case ${1:-list} in
  list)
    now=$(sum "$cx")
    staged | while read -r d; do
      name=${d:t}; for k v in ${(kv)alias}; do [[ $v == $d ]] && name="$k ($name)"; done
      printf '%s %-22s %s\n' "$([[ $(sum "$d") == $now ]] && print '*' || print ' ')" "$(version "$d")" "$name"
    done ;;
  status) print "Installed: $(version "$cx") (d3d11.dll ${$(sum "$cx"):0:12}) in $cx" ;;
  -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//' ;;
  *)
    src=$(resolve "$1")
    pgrep -f -q '[S]oulcaliburVI.exe' && { print -u2 "The game is running. Quit it first."; exit 1; }
    # A staged folder with a checksum file must still match it.
    for list in "$src"/SHA1SUMS(N) "$src"/SHA256SUMS(N); do
      algo=1; [[ ${list:t} == SHA256SUMS ]] && algo=256
      (cd "$src" && shasum -a $algo -c --quiet "$list") || { print -u2 "Checksum mismatch in $src"; exit 1; }
    done
    if same "$src" "$cx"; then
      print "Already installed: $(version "$cx") from $src"; exit 0
    fi
    # Keep the installed set unless a staged copy of it exists.
    if ! is_staged "$(sum "$cx")"; then
      backup="$data/backups/crossover-preview-dxmt-aarch64-$(date +%Y%m%d-%H%M%S)"
      mkdir -p "$backup"/aarch64-{windows,unix}
      for f in $files; do cp -p "$cx/$f" "$backup/$f"; done
      (cd "$backup" && shasum $files > SHA1SUMS)
      print "Backed up the installed build to $backup"
    fi
    for f in $files; do
      cp -p "$src/$f" "$cx/$f.swap-new"
      mv -f "$cx/$f.swap-new" "$cx/$f"   # a rename, so Wine never maps a half-written file
    done
    same "$src" "$cx" || { print -u2 "Copy check failed in $cx"; exit 1; }
    print "Installed $(version "$cx") from $src" ;;
esac
