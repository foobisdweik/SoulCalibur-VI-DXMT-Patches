#!/bin/zsh
# Downloads the external dependencies that deps.lock pins, and checks each download against its pin.
#
# Usage: tools/fetch-deps.sh [--check] [--force] [--allow-unpinned] [all | <group> | <name> ...]
#   With no names it fetches the "build" group, which the DXMT build needs.
#   --check           only report the state of each dependency; download nothing
#   --force           replace a dependency whose folder holds another version. The old folder is moved aside, not deleted
#   --allow-unpinned  fetch a dependency whose pin is "-", and print its hash so it can be added to deps.lock
#
# Each fetched folder gets a .deps-lock file with the name, version and pin, so a later run can tell
# which version a folder holds without downloading it again.
emulate -L zsh
setopt err_exit no_unset pipe_fail
zmodload zsh/datetime  # EPOCHSECONDS, for the names of moved-aside folders
# DEPS_ROOT and DEPS_LOCK let a test run use its own folder and lock file.
root=${DEPS_ROOT:-${0:A:h:h}}
lock=${DEPS_LOCK:-$root/deps.lock}

check=0 force=0 unpinned=0
typeset -a wanted
for arg in "$@"; do
  case $arg in
    --check) check=1 ;;
    --force) force=1 ;;
    --allow-unpinned) unpinned=1 ;;
    -h|--help) sed -n '2,10p' ${0:A} | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) print -u2 "unknown option: $arg"; exit 2 ;;
    *) wanted+=$arg ;;
  esac
done
(( $#wanted )) || wanted=(build)

failed=0
say() { printf '%-18s %s\n' $1 $2 }

# Prints the state of one dependency: ok, missing, other, unknown (present but not fetched by this script) or manual.
state() {
  local kind=$1 version=$2 pin=$3 dir=$4 name=$5
  [[ -e $dir ]] || { print missing; return }
  [[ $kind == manual ]] && { print manual; return }
  if [[ -f $dir/.deps-lock ]]; then
    [[ $(<$dir/.deps-lock) == "$name $version $pin" ]] && print ok || print other
  elif [[ $kind == git && -d $dir/.git ]]; then
    # A clone made before this script existed can still be checked by its commit.
    [[ $(git -C $dir rev-parse HEAD 2>/dev/null) == $pin ]] && print ok || print other
  else
    print unknown
  fi
}

fetch() {
  local name=$1 kind=$2 version=$3 pin=$4 dir=$5 strip=$6 url=$7
  local part=$dir.partial file=$dir.download
  # A partial folder is what an interrupted earlier run left. It is never a finished dependency.
  [[ -e $part ]] && mv $part $part.stale-$EPOCHSECONDS
  mkdir -p ${dir:h}
  case $kind in
    tar|zip)
      say $name "downloading $url"
      curl -fL --retry 3 --progress-bar -o $file $url
      local got=$(shasum -a 256 $file | awk '{print $1}')
      if [[ $pin == - ]]; then
        say $name "not pinned. SHA-256 of this download: $got"
      elif [[ $got != $pin ]]; then
        say $name "FAILED: SHA-256 is $got, but deps.lock pins $pin. The download is kept at $file"
        return 1
      fi
      mkdir $part
      if [[ $kind == tar ]]; then
        tar -xf $file --strip-components $strip -C $part
      else
        ditto -x -k $file $part
      fi
      rm $file ;;
    git)
      # GitHub serves a single commit by its hash, so a shallow fetch is enough.
      say $name "cloning $pin from $url"
      git init -q $part
      git -C $part fetch -q --depth 1 $url $pin
      git -C $part checkout -q --detach FETCH_HEAD
      [[ $(git -C $part rev-parse HEAD) == $pin ]] || { say $name "FAILED: the clone is not at $pin"; return 1 } ;;
  esac
  print -r -- "$name $version $pin" > $part/.deps-lock
  mv $part $dir
  say $name "ok ($version)"
}

matched=0
while read -r name group kind version pin dest strip url; do
  [[ -z $name || $name == \#* ]] && continue
  (( ${wanted[(Ie)all]} || ${wanted[(Ie)$group]} || ${wanted[(Ie)$name]} )) || continue
  matched=1
  dir=$root/$dest
  st=$(state $kind $version $pin $dir $name)
  if (( check )); then
    case $st in
      ok) say $name "ok ($version)" ;;
      missing) say $name "missing: run tools/fetch-deps.sh $name" ;;
      other) say $name "holds another version than $version: run tools/fetch-deps.sh --force $name" ;;
      unknown) say $name "present, but not fetched by this script, so its version is unverified" ;;
      manual) say $name "present (placed by hand; deps.lock says how to get it)" ;;
    esac
    [[ $st == (missing|other) ]] && failed=1
    continue
  fi
  case $st in
    ok) say $name "ok ($version)"; continue ;;
    unknown|manual) say $name "present, so not fetched again (its version is unverified)"; continue ;;
    other)
      if (( ! force )); then
        say $name "holds another version than $version. Run with --force to replace it"; failed=1; continue
      fi
      mv $dir $dir.replaced-$EPOCHSECONDS
      say $name "moved the old folder to ${dest}.replaced-$EPOCHSECONDS. Delete it when the new one works" ;;
  esac
  if [[ $kind == manual ]]; then
    say $name "missing. There is no public download; see the comment above it in deps.lock"; failed=1; continue
  fi
  if [[ $pin == - ]] && (( ! unpinned )); then
    say $name "not pinned in deps.lock. Run with --allow-unpinned to fetch it and print its hash"; failed=1; continue
  fi
  fetch $name $kind $version $pin $dir $strip $url || failed=1
done < $lock

(( matched )) || { print -u2 "no dependency or group named: $wanted"; exit 2 }
exit $failed
