#!/usr/bin/env bash
# Run a detached fuzz campaign: a GUI/MCP fuzz pass, then a leak-mode pass, appending progress lines.
# Safe to start with setsid/nohup so it survives a closed terminal:
#
#   setsid nohup fuzz/run_campaign.sh > /dev/null 2>&1 < /dev/null &
#
# Env: TAG (default pass1), MINUTES (default 50), LEAK_MINUTES (default 10, 0 = skip), plus everything
# fuzzcfg.py reads (PRINTBOT_FUZZ_APP, PRINTBOT_FUZZ_WORK, ...). Extra args are passed to fuzz.py.
set -u
FZ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$FZ/.." && pwd)"
WORK="${PRINTBOT_FUZZ_WORK:-$REPO/temp/fuzz}"; mkdir -p "$WORK"
TAG="${TAG:-pass1}"; MINUTES="${MINUTES:-50}"; LEAK_MINUTES="${LEAK_MINUTES:-10}"
P="$WORK/PROGRESS.txt"
note() { echo "$(date +%T) $*" >> "$P"; }
[ -n "$(ls -A "${PRINTBOT_FUZZ_CORPUS:-$WORK/corpus}" 2>/dev/null)" ] || python3 "$FZ/gen_corpus.py" >> "$WORK/gen_corpus.log" 2>&1
note "$TAG: fuzz start ($MINUTES min)"
python3 "$FZ/fuzz.py" --minutes "$MINUTES" --tag "$TAG" --fresh-prob 0.1 "$@" > "$WORK/fuzz_$TAG.log" 2>&1
note "$TAG: fuzz done rc=$? stats: $(cat "$WORK/stats_$TAG.json" 2>/dev/null | tr -d '\n ' | cut -c1-300)"
if [ "$LEAK_MINUTES" != "0" ]; then
    sleep 5
    note "${TAG}_leaks: start ($LEAK_MINUTES min)"
    python3 "$FZ/fuzz.py" --minutes "$LEAK_MINUTES" --leaks --tag "${TAG}_leaks" --fresh-prob 0.1 "$@" > "$WORK/fuzz_${TAG}_leaks.log" 2>&1
    note "${TAG}_leaks: done rc=$?"
    python3 "$FZ/leak_summary.py" "$WORK/runs_${TAG}_leaks/*/ubsan.*" > "$WORK/leaks_${TAG}.txt" 2>&1
    note "${TAG}_leaks: summary in $WORK/leaks_${TAG}.txt"
fi
