#!/bin/zsh
# Record the CPU use of every SoulcaliburVI.exe thread, by name, for a time window, together with DXMT's frame log
# lines (DXMT_SC6_FRAME_LOG) from the same window. Nothing pauses the game.
#
#   capture-threads.sh [label] [seconds]     default: "capture", 15 seconds
#
# Output: $data/logs/threads-<time>-<label>/threads.tsv (thread_cpu_mac.c) and frames.tsv.
# Thread names need SC6VRR_THREAD_NAMES=1 or SC6VRR_SAMPLE=1 in sc6vrr.ini, set before the game starts.
set -euo pipefail
here=${0:A:h}
label=${1:-capture}
seconds=${2:-15}
data="$HOME/Library/Application Support/SC6-VRR"
frames="$data/pass-stats/SC6-FEX-frames.tsv"
tool="$here/bin/thread_cpu_mac"
if [[ ! -x $tool || $here/thread_cpu_mac.c -nt $tool ]]; then
  mkdir -p "$here/bin"
  clang -O2 -Wall -o "$tool" "$here/thread_cpu_mac.c"
fi
pid=$(pgrep -f 'Binaries\\Win64\\SoulcaliburVI\.exe' | head -1) || { print -u2 "SoulcaliburVI.exe is not running"; exit 1; }
out="$data/logs/threads-$(date +%Y%m%d-%H%M%S)-$label"
mkdir -p "$out"
t0=$(( $(date +%s) * 1000000 ))
"$tool" "$pid" "$seconds" > "$out/threads.tsv"
t1=$(( $(date +%s) * 1000000 ))
# frame log column 2 is the present time in unix microseconds
[[ -f $frames ]] && awk -F'\t' -v a=$t0 -v b=$t1 'NR == 1 || ($2 >= a && $2 <= b)' "$frames" > "$out/frames.tsv"
print "saved $out"
head -16 "$out/threads.tsv"
if [[ -s $out/frames.tsv ]]; then
  awk -F'\t' 'NR > 1 { n++; dt += $3; enc += $10 + $11; pw += $5; dw += $6; if ($3 > 12) slow++ }
    END { if (n) printf "frames=%d avg_dt_ms=%.2f (%.1f fps) encode_ms=%.2f present_wait_ms=%.2f drawable_wait_ms=%.2f frames_over_12ms=%d\n",
          n, dt / n, 1000 * n / dt, enc / n, pw / n, dw / n, slow }' "$out/frames.tsv"
fi
