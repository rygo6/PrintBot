# Fuzzing status

Last updated: 2026-11-18. Harness and usage: [README.md](README.md).

## Summary

- The Linux GUI and MCP endpoint were fuzzed under clang ASan+UBSan for about 3 hours in total, across 8
  campaigns plus 2 leak campaigns.
- Every crash, UB report and repo-owned leak found has been fixed and re-verified with `repro.py`.
- The last three confirmation passes (`final`, `final2`, `confirm4`) had 0 crashes and no new sanitizer reports. The last
  leak-mode repro found 0 repo-owned leaks; everything reported came from wx/GTK/fontconfig/GL driver code.
- Windows (MSVC `/fsanitize=address`): the build and a smoke run work. Windows fuzzing is partial (see Open
  items).
- macOS (Apple clang ASan+UBSan): harness added (`fuzz_mac.py`, `build_asan_mac.sh`), see the macOS section.

## Builds

**Linux ASan+UBSan** (`build_asan.sh`)
- Toolchain: clang 21.1.8, lld, Ninja, `Release` with
  `-fsanitize=address,undefined -fno-omit-frame-pointer`, `SLIC3R_STATIC=1`, `SLIC3R_GTK=3`.
- **TBB had to be rebuilt with clang.** The deps build compiles TBB with GCC + LTO, and lld cannot link GCC LTO
  objects. The first ASan link failed for that reason. TBB was rebuilt once with clang (static, no LTO) into a
  separate prefix that is searched first via `CMAKE_PREFIX_PATH` / `TBB_DIR`.
- **Precompiled headers.** Build with `SLIC3R_PCH=ON`, the tested configuration. A no-PCH build originally
  failed because some headers relied on the PCH for includes (e.g. `ClipperZUtils.hpp` needed `ExPolygon.hpp`).
  Those includes were added, and the clang `-Wreturn-stack-address` / `-Wformat-security` warnings were cleaned
  up.
- The normal (non-sanitized) build supplies the `print_bot-mcp` relay. Both builds are rebuilt after each fix
  round and were green at the end.

**Windows** (`fuzz_win.py`)
- MSVC `RelWithDebInfo` with `/fsanitize=address`. The runtime uses `windows_hook_rtl_allocators=0`.
- The GLEW dependency had to be built on Windows, since there is no system copy.
- The MCP socket needed a Windows implementation: AF_UNIX through Winsock, a per-user directory, and a check
  that each peer process runs as the same user. That port is now in the tree: the socket lives at
  `%LOCALAPPDATA%\PrintBot\mcp.sock`, and the MCP screenshot tool has a Windows implementation.

## Harness

- **UI side:** random X input on Xvfb (clicks, drags, wheel, keys and shortcuts, typed values including
  `-1`, `999999`, `1e10`, `nan`, `%`, unicode), window resizes down to tiny sizes, and automatic handling of
  file choosers and modal dialogs.
- **MCP side:** random tool calls, with arguments generated from each tool's JSON schema plus hostile values,
  sent concurrently with the UI actions. Printer and print-job tools are filtered out.
- **Input corpus** (`gen_corpus.py`): valid models from the repo plus malformed STL/OBJ/STEP/3MF variants and
  mutated Bambu project 3MFs.
- **Dialog coordination:** about 85% of state-changing MCP calls are sent after the fuzzer pauses UI input and
  closes dialogs. The other ~15% are deliberately sent while a modal is open, to test the re-entrancy guard.
- **Leak mode:** see README.

## Campaigns

| Campaign | Phase | Runs | UI actions | MCP calls | Result |
|---|---|---|---|---|---|
| smoke | 1 | 3 | 328 | 152 | `create_scaled_bitmap` null window, which blocked every run until fixed |
| pass1 (`main`) | 1 | 29 | 8374 | 2964 | 26 runs ended in a sanitizer abort, 16 unique issues (below) |
| pass2 | 2 | 7 | 7072 | 3782 | 0 crashes after the phase-2 fixes |
| leaks (10 min) | 2 | 3 | 1397 | 745 | Repo-owned leaks: 3MF parser, placeholder parser, plate textures, … |
| pass3 (50 min) | 3 | 11 | 3315 | 1799 | 1 crash (context-menu UAF) plus UB: range overflow, clipper overflow, slider cast |
| leaks3 (10 min) | 3 | 3 | 663 | 313 | Undo-history copy, 3MF export buffer, dialog leaks |
| confirm | 3 | 14 | 0 | 0 | Harness failure (Xvfb display race between repro and fuzzer), no app result |
| confirm2 (15 min) | 3 | 3 | 1065 | 595 | 0 crashes; 1 UB (`load_bbs_3mf` null outputs) |
| final (12 min) | 3 | 3 | 871 | 461 | 0 crashes; 1 UB (ImGui size overflow at tiny window size) |
| final2 (10 min) | 4 | 2 | 718 | 384 | 0 crashes, 0 new reports, 36 MCP calls refused by the re-entrancy guard |
| confirm4 (15 min) | 5 | 2 | 1088 | 530 | 0 crashes, 0 hangs, 0 new reports; 57 MCP calls refused by the re-entrancy guard |
| Windows smoke | – | – | – | – | App launched and an MCP-driven slice completed; the long pass did not finish |

Repro: `repro.py` replays every finding below; it has about 100 MCP steps. `--section slice_idle` and
`--section validate_image` run single focused sections. The last full run (in leak mode, phase 5) completed
with the app alive throughout, 0 ASan/UBSan reports and 0 repo-owned leaks.

## Findings

All of these are fixed and verified with `repro.py` unless noted.

### Memory errors and crashes

| Type | Location | Fix |
|---|---|---|
| heap-use-after-free | `Fill/FillFloatingConcentric.cpp` (~698): reference into a temporary vector | Keep the vector as a named local |
| heap-use-after-free | `FillFloatingConcentric.cpp` / `VariableWidth.cpp`: `line = lines[i]` held by reference while `lines` is modified | Copy the element |
| heap-use-after-free | `Plater.cpp` (~13999): timelapse lambdas kept pointers into a temporary `full_config()` | Capture configs by value |
| heap-use-after-free | `PartPlate::is_locked` via `GUI_Factories.cpp`: MCP `undo` while a right-click menu was open | Re-entrancy guard refuses while a popup menu is open (`ui.busy`) |
| SEGV / null ref | `Model.cpp` (~2960-2996, ~3610): volume with no convex hull | Return an empty mesh, null checks |
| SEGV / null ref | `GCode.cpp` (~2559), `Print.hpp:209`: G-code export with no objects | Clean "Nothing to print" error |
| null ref | `GCode.cpp` (~3665): spiral-vase lambda dereferenced a null `m_spiral_vase` | Capture the pointer and check it |
| null member call | `PrintBotMcpTools.cpp` (~264): `select_plate` with plate 0 | Plate index validated; omitted = current plate; null check |
| null ref | `bbs_3mf.cpp` (~9744): "Replace with STL" given a `.3mf` passed null output pointers | Local fallbacks for the optional outputs |
| heap overread | `read_binary_stl` header parsing (MakerWorld/MakerLab ID) | Parsing removed |
| null member call | `wxExtensions.cpp:501` (`create_scaled_bitmap` from `BBLTopbar`) | Pass the owning window; static `wxWindow::FromDIP` |

### Undefined behaviour

| Type | Location | Fix |
|---|---|---|
| Uninitialised bool/enum loads | `ImageSwitchButton`, `MediaPlayCtrl`, `Monitor`/`StatusPanel` pointers, `Plater` priv ordering | Member initialisers; `priv` created in the ctor body |
| `vector<bool>` element UB | `PrintConfig.cpp` (~9065, ~9257) | Copy through `unsigned char`; also a copy-paste bug in `compute_filament_override_value` |
| NaN → int | `tiny_bvh.h` (~365, ~2419), `PrintConfig.cpp` (~9630) | Skip BVH build for non-finite boxes; NaN guard; NaN/inf vertices rejected at import |
| int overflow | Eigen `AlignedBox` squared distance (via `AABBTreeIndirect.hpp`) | Distance computed in `double` |
| int overflow | `GLCanvas3D.hpp:380` sentinel, mouse move threshold | 64-bit math |
| -1 → unsigned | `GLCanvas3D.cpp` (~9198) G-code moves slider | Clamp to 0 |
| int overflow | `PerimeterGenerator.cpp`, `LayerRegion.cpp`, `RegionExpansion.cpp` via MCP `wall_loops=99999` | MCP setting values checked against each option's min/max (`preset.bad_value`) |
| Clipper int overflow | `clipper.cpp` via `PrintObject::discover_vertical_shells`, from a huge value typed into the GUI | Inputs bounded, see "Range guards" |
| float → int overflow | `imgui.cpp:7396` (`ItemSize`): notification link button in a tiny window | Button size and text position sanitised (`ImGuiWrapper::button`, `render_hypertext`) |

### Leaks (LeakSanitizer, repo-owned)

| Location | Fix |
|---|---|
| `bbs_3mf.cpp` (~2767): miniz buffer when an XML callback throws | Callbacks store the exception, stop miniz, and rethrow after cleanup |
| `bbs_3mf.cpp` (~2494-2526), `m_curr_plater`: plate data on error paths / duplicate plates | `discard_unowned_plate`; `ScopeGuard` in `Plater::priv::load_files` |
| `bbs_3mf.cpp` (~7620): 1 MB per object in the 3MF export heap archive | `mz_free` the finalized buffer |
| `Model.cpp` (~512): `check_3mf_from_prusa` zip reader not closed | Close on all paths |
| `PlaceholderParser.cpp` (~223): string expressions (`operator=`, `compare_op`) | Free before overwrite; self-assignment checks |
| `UndoRedo.cpp` (~881): `ModelInstance` copy leaked per undo/redo | `Model::collect_reusable_objects` also offers instances for reuse |
| `PartPlate.cpp` (~8052), `GLTexture.cpp` (~1194): app-lifetime textures | Owned and released |
| `DailyTipsPanel` renderer, `MainFrame.cpp` (~3079) menu item, `StatusPanel.cpp` unattached sizers, `KBShortcutsDialog` selectors, `ProjectDropDialog` radio selectors, `ImageMessageDialog` `wxImage` | `unique_ptr`/destructors/stack objects; unused sizers removed |

### MCP re-entrancy and range guards

- **Re-entrancy.** MCP tool calls run on the GUI thread through `CallAfter`, which also fires inside nested modal
  event loops. That let state-changing calls run underneath open dialogs and menus, which caused crashes. A
  state-changing call is now refused when:
  - a modal dialog is open (`ui.blocked_by_dialog`, naming the dialog),
  - another MCP call is still in progress (`ui.busy`), or
  - a popup/context menu is open (`ui.busy`).

  Read-only and dialog tools (`ui_list_dialogs`, `ui_close_dialog`, …) are still allowed, so a client can answer
  the dialog.
- **Range guards** (the coordinates used while slicing are 32-bit, about ±21 m):
  - MCP `set_preset_settings` / `set_object_settings` reject values outside an option's declared min/max.
  - MCP `transform_object` rejects a position or move beyond ±5000 mm, a size over 5000 mm, and a scale over
    1000 or non-finite.
  - Typing into the object manipulation fields ignores non-finite values and positions, sizes or scales that
    would exceed 5000 mm.
  - `Print::validate` refuses to slice an object whose bounding box exceeds ±5000 mm, and any mm/% setting
    whose magnitude is absurd (over 5000 mm or over 100000%). This catches values with no declared maximum
    that reach slicing through any route.

## Resolved since the first report (phase 5)

- **Slice sitting idle after an export error.** After G-code export failed (e.g. a bad placeholder in the end
  G-code), the error dialog came up during the MCP `slice` wait, and the call reported a misleading "nothing
  was done". A second `slice` waited out its whole timeout, because the background process does not restart
  when nothing has changed since the failure. `slice` now returns `slice.error` straight away in both cases:
  it names the dialog (read or close it with the `ui_*` tools), or says that plate N failed earlier and nothing
  has changed since. Verified with `repro.py --section slice_idle`: the second slice returns in about 8 s
  instead of timing out, and a slice after fixing the G-code succeeds.
- **`ImageMessageDialog` leak path exercised.** The repro now enables the wrapping-detection prompt and switches
  to a printer that shows it ("Parameter recommendation"). The dialog opens and is closed, and the leak-mode
  runs report 0 repo-owned leaks.
- **`Print::validate` setting guard exercised.** `xy_contour_compensation` and similar process values are
  already reset by the app above small limits (with an info dialog), so the repro uses
  `top_shell_thickness=999999` (process) and `xy_contour_compensation=999999` (per object, which bypasses that
  reset). Both are refused before slicing, and the MCP `slice` error now names the reason and the setting
  (`slice.invalid_plate: ... A setting has a value far outside its usable range ... (setting
  'top_shell_thickness')`). Resetting the value makes the plate slice again.
- **Windows MCP port** (socket, screenshot, GLEW dependency) is in the tree. The Linux builds were rebuilt with
  it and re-tested.

## macOS

`fuzz_mac.py` drives the app only through MCP (no synthesized global input) under `sandbox-exec` with IP
networking denied, and `build_asan_mac.sh` configures the Apple clang ASan+UBSan build. LeakSanitizer is not
available on Apple silicon. No macOS fuzz results are recorded yet.

## Open items

1. **The extreme-coordinate overflow code is unchanged.** Coordinates around 1e7 mm overflow in `Model.cpp`
   (~3531), `Point.hpp` (~197) and `Geometry.hpp` (~42) before validation runs. Those paths can now only be
   reached past the input bounds above (MCP, GUI fields, validate). The code itself was not changed.
2. **Windows fuzzing is partial.** The build and a smoke run (launch plus an MCP-driven slice) worked. The long
   pass stopped when the Windows test machine reset. The reset was a CPU machine-check (WHEA) hardware error,
   not PrintBot. No Windows findings have been recorded. Rerun `run_fuzz.cmd` on a stable machine.
3. **macOS fuzzing has not run yet** (see above).
4. **Harness.** Run fuzzers and `repro.py` one at a time on a given display. Both wait for the X lock, but
   killing a run mid-way can leave a stale lock or a slow-exiting Xvfb.
