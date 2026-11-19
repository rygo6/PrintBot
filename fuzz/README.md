# PrintBot fuzz harness

Chaos fuzzers that drive the PrintBot GUI (random clicks, keys, typing, window resizes, dialogs, file choosers)
and its MCP endpoint (random tool calls with schema-shaped random arguments, sent concurrently with the UI
actions) against a sanitizer build, then deduplicate sanitizer reports into crash directories.
They are Python 3 scripts (standard library only) plus a small C shim for leak checks.
Current results and every finding are in [FUZZ_STATUS.md](FUZZ_STATUS.md).

| File | Purpose |
|---|---|
| `fuzz.py` | Linux fuzzer (Xvfb + xdotool + MCP), ASan/UBSan build. `--leaks` = leak mode |
| `repro.py` | Replays every known finding through MCP and records per-step results |
| `gen_corpus.py` | Builds the input corpus (valid models from the repo + malformed STL/OBJ/STEP/3MF) |
| `fuzzcfg.py` | Shared paths/settings; repo-relative defaults, env overrides |
| `lsan_trigger.c` | `LD_PRELOAD` shim: runs a recoverable LeakSanitizer check when a trigger file appears |
| `leak_summary.py` | Splits leak reports into repo-owned (`file:line`) vs library noise |
| `mcp_stats.py` | MCP outcome counts per campaign (ok / error / refused by the re-entrancy guard) |
| `shot.py` | Screenshot helper (PyGObject) used for crash context |
| `ubsan.supp` | UBSan suppressions for third-party code (miniz alignment, GLU tessellator) |
| `build_asan.sh` | Configure + build the clang ASan+UBSan build (and the clang TBB it needs) |
| `run_campaign.sh` | Detached campaign: fuzz pass + leak pass + leak summary, with progress lines |
| `fuzz_win.py`, `run_fuzz.cmd` | Windows fuzzer for the MSVC `/fsanitize=address` build |
| `fuzz_mac.py`, `build_asan_mac.sh` | macOS fuzzer (MCP-driven, sandboxed) and its Apple clang ASan+UBSan build |

All output (runs, crash dirs, sandboxes, seed profile, corpus, stats) goes to `temp/fuzz/` under the repo
by default. Set `PRINTBOT_FUZZ_WORK` or pass `--work` to change it. Other overrides: `PRINTBOT_FUZZ_APP`,
`PRINTBOT_FUZZ_MCP`, `PRINTBOT_FUZZ_CORPUS`, `PRINTBOT_FUZZ_DISPLAY`, and `PRINTBOT_FUZZ_SYSROOT` (an optional
user-space sysroot that provides `xdotool`). See `fuzzcfg.py` for details.

## Safety

- **Printer and print-job MCP tools are never called.** `fuzzcfg.BLOCKED_TOOLS` lists them explicitly, and
  any tool whose name looks printer/job related (`printer`, `print_plate`, `send_`, `upload`, `camera`,
  `authoriz`) is filtered too, so new tools are excluded by default. A fuzzer must never be able to start,
  stop or change a real print, move axes or heat anything.
- **Linux: no network.** The app runs as `unshare -cn` (unprivileged user + network namespace, loopback
  only). The fuzzer refuses to start if that is unavailable.
- Every run uses a throwaway profile (`HOME`/`XDG_*` in a sandbox dir), D-Bus is disabled, and
  `xdg-open`/browsers are replaced by stubs that only log the URL.
- No sudo is needed. Input goes only to the Xvfb display (Linux) or to PrintBot's own windows via
  `PostMessage` (Windows).

## Linux

Requirements: clang + lld (tested with clang 21), Ninja, `Xvfb`, `xdotool`, `xprop`/`xwininfo`, Python 3,
PyGObject (screenshots only), `unshare` with unprivileged user namespaces.

```sh
fuzz/build_asan.sh                        # -> build-asan/src/bambu-studio (deps must already be built)
cmake --build build                       # normal build: provides the print_bot-mcp relay
python3 fuzz/gen_corpus.py                # -> temp/fuzz/corpus
python3 fuzz/fuzz.py --minutes 60 --tag pass1
python3 fuzz/fuzz.py --minutes 10 --leaks --tag leaks1      # leak mode
python3 fuzz/leak_summary.py 'temp/fuzz/runs_leaks1/*/ubsan.*'
python3 fuzz/repro.py                     # replay known findings; --leaks for the leak-mode variant
setsid nohup fuzz/run_campaign.sh >/dev/null 2>&1 </dev/null &   # long detached campaign
```

Notes:
- The fuzzer starts its own `Xvfb :77` (`--display` to change it) and waits if that display is still locked.
- The first healthy run saves its profile as `temp/fuzz/seed_home`. Later runs start from it (or from an empty
  profile with probability `--fresh-prob`), and `repro.py` uses it.
- Unique findings go to `temp/fuzz/crashes_<tag>/cNNN/` (`report.txt` and `actions_before.jsonl`, the action
  log leading up to the report). The index is `uniq.json`. Stats go to `stats_<tag>.json`.
- Before most state-changing MCP calls, the fuzzer pauses the X actions and closes open dialogs, so calls are
  not all refused by the re-entrancy guard. About 15% are deliberately sent while a modal is open, to test that
  guard.
- Leak mode: `detect_leaks=1`, and each run ends by touching a trigger file. `lsan_trigger.so` (built
  automatically from `lsan_trigger.c` into the work dir) then runs `__lsan_do_recoverable_leak_check()` from a
  normal thread and exits. This is needed because the GUI rarely reaches `atexit` cleanly under a fuzzer.

## Windows

The Windows build is MSVC with `/fsanitize=address`. It needs a build whose MCP socket supports Windows
(AF_UNIX through Winsock).

```bat
python fuzz\gen_corpus.py
fuzz\run_fuzz.cmd --minutes 40 --tag win
```

Defaults: `--app build-asan\src\RelWithDebInfo\bambu-studio.exe`, `--mcp build\src\Release\print_bot-mcp.exe`,
output `temp\fuzz`. Each run uses `--datadir` in a sandbox plus private `APPDATA`/`LOCALAPPDATA`/`TEMP`, and
the same printer-tool filter. There is no network namespace on Windows, so use a machine with no printer on
its network, or block the app in the firewall.

## macOS

The macOS build uses Apple clang with `-fsanitize=address,undefined`. LeakSanitizer is not supported on Apple
silicon, so macOS runs check for memory errors and UB only.

```sh
./BuildMac.sh -d -x                       # deps, once
fuzz/build_asan_mac.sh                    # -> build-asan/src/BambuStudio.app and print_bot-mcp
python3 fuzz/gen_corpus.py
python3 fuzz/fuzz_mac.py --minutes 45 --tag mac
```

- The app is driven only through MCP, including the `ui_*` tools (tabs, dialogs, camera). No global mouse or
  keyboard events are synthesized, so nothing reaches other apps and no Accessibility or Screen Recording
  permission is needed.
- Each run gets its own `--datadir`, `HOME`, `TMPDIR` and MCP socket. The app runs under `sandbox-exec` with all
  IP networking denied (Unix sockets still work), so it cannot reach printers or cloud services. No sudo is
  needed.
- Output layout matches the Linux fuzzer: `temp/fuzz/crashes_<tag>/`, `stats_<tag>.json`, `progress_<tag>.txt`.
