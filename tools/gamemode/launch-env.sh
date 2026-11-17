# Source this file after setting cx_root and repo. It prepares Wine's --env value.
sc6_gamemode_helper=${SC6_GAMEMODE_HELPER:-$HOME/.local/bin/crossover-gamemode}
[[ -x $sc6_gamemode_helper ]] || {
  print -u2 "The Game Mode helper is missing: $sc6_gamemode_helper"; return 1; }
sc6_dxmt_files=({d3d10core,d3d11,dxgi,winemetal,nvapi64,nvngx}.dll)
# An FEX bottle (such as SC6-fex) loads Preview's aarch64 DXMT, which
# tools/dxmt-swap.sh installs. A Rosetta bottle loads the x86_64 DXMT.
sc6_dxmt_arch=${SC6_DXMT_ARCH:-$([[ ${SC6_BOTTLE:-} == *[Ff][Ee][Xx]* ]] && print aarch64 || print x86_64)}
sc6_same_dxmt() {  # $1: a folder with <arch>-windows/ and <arch>-unix/
  local f
  for f in $sc6_dxmt_files; do
    cmp -s "$1/$sc6_dxmt_arch-windows/$f" "$cx_root/lib/dxmt/$sc6_dxmt_arch-windows/$f" || return 1
  done
  cmp -s "$1/$sc6_dxmt_arch-unix/winemetal.so" "$cx_root/lib/dxmt/$sc6_dxmt_arch-unix/winemetal.so"
}
if [[ $sc6_dxmt_arch == aarch64 ]]; then
  # The installed set must match one staged build, so a run names its build.
  sc6_dxmt_runtime=${SC6_DXMT_RUNTIME:-}
  if [[ -z $sc6_dxmt_runtime ]]; then
    for sc6_dir in "$HOME/Library/Application Support/SC6-VRR/builds"/*(N/); do
      sc6_same_dxmt "$sc6_dir" && { sc6_dxmt_runtime=$sc6_dir; break; }
    done
  fi
  [[ -n $sc6_dxmt_runtime ]] && sc6_same_dxmt "$sc6_dxmt_runtime" || {
    print -u2 "Preview's aarch64 DXMT matches no staged build. Run $repo/tools/dxmt-swap.sh <name>."
    return 1
  }
  print -u2 "DXMT (aarch64): ${sc6_dxmt_runtime:t}"
else
  sc6_dxmt_runtime=${SC6_DXMT_RUNTIME:-$HOME/Library/Application Support/SC6-VRR/runtimes/dxmt-direct-x86_64}
  [[ -f $sc6_dxmt_runtime/x86_64-unix/winemetal.so ]] || {
    print -u2 "Run $repo/tools/gamemode/install-direct-dxmt.sh first."; return 1; }
  sc6_same_dxmt "$sc6_dxmt_runtime" || {
    print -u2 "Preview's x86_64 DXMT differs from the staged build. Run $repo/tools/gamemode/install-direct-dxmt.sh."
    return 1
  }
fi

# The [launch] section of sc6vrr.ini (next to SoulcaliburVI.exe) sets most
# values below. Without the file, the defaults match the earlier fixed list.
sc6_ini=${SC6_INI:-${win64:-}/sc6vrr.ini}
source "${${(%):-%x}:A:h}/ini-env.sh"
sc6_env_list=(
  "WINEDLLOVERRIDES=d3d11,dxgi,winemetal=b"
  "CX_GRAPHICS_BACKEND=dxmt"
  "${sc6_launch_env[@]}"
)
# CrossOver resets WINELOADER for its own launches, so the Game Mode helper
# works only for launches through these scripts.
[[ $(sc6_ini_get launch GameModeHelper 1) == 1 ]] && sc6_env_list+=(
  "WINELOADER=$sc6_gamemode_helper" "CX_GAMEMODE_PROCESS=SoulcaliburVI.exe" "CX_GAMEMODE_DRY_RUN=0")

# CrossOver parses quoted assignments itself. Escape their values before passing them.
sc6_encode_env() {
  local kv key value
  sc6_env_arg=""
  for kv in "$@"; do
    key=${kv%%=*}; value=${kv#*=}
    value=${value//\\/\\\\}; value=${value//\"/\\\"}
    sc6_env_arg+="$key=\"$value\" "
  done
}
