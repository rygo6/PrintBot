# print_bot — LAN-only fork of Bambu Studio

This tree is Bambu Studio (bambulab/BambuStudio master @ 79df7ae30, version
02.08.04.57, AGPL-3.0) with every Bambu Cloud feature removed and the closed
Bambu network plugin replaced by the open
[open-bamboo-networking](https://github.com/ClusterM/open-bamboo-networking)
plugin (vendored in `bambu_networking/`, AGPL-3.0-or-later; upstream commit in
`bambu_networking/UPSTREAM_COMMIT`).

Slicing, the Prepare/Preview tabs, printer/filament/process presets and all
LAN printer operations (discovery, add by IP + access code, upload, start,
pause/resume/stop, live status, camera, AMS, calibration) work as in stock
Bambu Studio. (The legacy Python GTK4 app was removed from the tree in t63u.)

## What is removed

All controlled by the CMake option `PRINTBOT_LAN_ONLY` (default `ON`), which
defines `PRINTBOT_LAN_ONLY=1` for the whole build and passes `OBN_LAN_ONLY` to
the plugin build:

- Bambu account login/logout, register, user avatar, printer binding/unbinding,
  ping-code binding, region device report.
- Cloud print, cloud print records, cloud print history, MakerWorld (home page
  models, model mall, publishing, ratings, model import), MakerLab, Maker's
  Supply, design staff picks, academy list.
- Cloud preset sync and user preset/setting upload, Filament Manager (cloud
  spool catalogue), Helio.
- Telemetry/tracking, consent reporting, privacy-version check.
- Studio and network-plugin update checks, automatic download/install of the
  closed plugin, Bambu network test dialog.
- HMS description downloads (bundled `resources/hms` data is used).
- In the plugin: cloud forced off (`block_cloud`), no app-certificate fetch,
  all cloud endpoints point at `cloud-disabled.invalid`.

Gating is done at three layers: `NetworkAgent` (no cloud ABI call reaches the
plugin), the GUI (menus, home page, dialogs, startup jobs), and the plugin.

### Project and Calibration tabs (deleted outright, not gated)

The top-level **Project** tab (`ProjectPanel`, the web-based model/project info
page) and **Calibration** tab (`CalibrationPanel` and the calibration wizards)
are deleted from the source tree, along with:

- `src/slic3r/GUI/Project.{cpp,hpp}`, `CalibrationPanel.*`, `CalibrationWizard.*`,
  `CalibrationWizard{Start,Preset,Cali,Save}Page.*`, `CaliHistoryDialog.*`,
  `calib_dlg.*` (Temperature / Flow rate / Pressure advance / Retraction /
  Max flowrate / VFA dialogs) and their `src/slic3r/CMakeLists.txt` entries.
- The "Calibration" toolbar drop-down / menu (topbar button on Linux and
  Windows, menubar menu on macOS) and its Tutorial link.
- `MainFrame`: `m_project`, `m_calibration`, the OrcaSlicer calibration dialog
  members, `show_calibration_button`, `update_calibration_button_status`,
  `get_calibration_curr_tab`, the project-page "unsaved changes" leave/close
  guard, and the `tpProject`/`tpCalibration`/`tpAuxiliary` tab ids. The
  remaining tab ids (Home, Prepare, Preview, Device, Multi-device) are
  unchanged.
- `Plater::calib_pa/flowrate/temp/max_vol_speed/retraction/VFA` and the
  calibration-job-finished event; the calibration-task branches in `PrintJob`.
- `CalibUtils`: the wizard-only print paths (calibration model generation,
  auto PA/flow-ratio MQTT drivers, pre-calibration checks, `send_to_print`).

Kept because Device/Monitor/AMS/presets still use them: `CalibrationWizardPage.*`
(shared `FilamentComboBox`, `CalibrationMethod`, `EVT_CALI_TRAY_CHANGED`),
`Calibration.*` (the Device tab's printer self-calibration dialog),
`ExtrusionCalibration.*`, the remaining `CalibUtils` helpers (PA profile
selection, K/N lookup, nozzle support lists), `DevCalib`, `libslic3r/Calib.*`
and all flow-ratio / pressure-advance preset fields.

Settings are stored in `~/.config/PrintBot` so they never mix with a stock
Bambu Studio install. App config defaults: `lan_mode_only=1`,
`sync_user_preset=false`.

## Printer requirements

The printer must be in **LAN Only** mode with **Developer Mode** enabled
(printer screen: Settings → Network/General). No Bambu certificates are needed
in that mode.

## Build (Linux)

    source temp/env.sh          # only needed while the user-space sysroot is used
    cmake -S deps -B deps/build -G Ninja -DDESTDIR=$PWD/deps/build/destdir \
        -DDEP_WX_GTK3=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build deps/build
    cmake -S . -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_PREFIX_PATH="$PWD/deps/build/destdir/usr/local" \
        -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DCMAKE_BUILD_TYPE=Release \
        -DBBL_RELEASE_TO_PUBLIC=1 -DBBL_INTERNAL_TESTING=0
    cmake --build build --target BambuStudio -- -j8

The open network plugin is built automatically (`printbot_networking`
target) and installed into `build/src/plugins/`, where the executable loads it
first.

## Run

    ./print_bot.sh

## MCP server (live app + headless fallback)

`build/src/print_bot-mcp` is the [MCP](https://modelcontextprotocol.io) entry point for agents
(stdio, newline-delimited JSON-RPC 2.0; protocol 2025-06-18, 2025-03-26, 2024-11-05). It is built
with the `BambuStudio` target (or alone: `cmake --build build --target print_bot-mcp`).

**Live mode (default).** The running print_bot app hosts an MCP endpoint on a private Unix
socket: `$PRINTBOT_MCP_SOCKET`, else `$XDG_RUNTIME_DIR/print_bot/mcp.sock`, else
`/tmp/print_bot-<uid>/mcp.sock` (directory 0700, socket 0600, peer uid checked). `print_bot-mcp`
connects to it and just relays bytes, so the agent drives the window the user is looking at.
Every tool runs on the wx main thread through the same code paths as the user's clicks (preset
combo boxes, settings tabs, the move/rotate/scale manipulation, toolbar events, tabs and
dialogs), so the UI shows everything the agent does; each state-changing call also shows an
"Agent: ..." notification on the canvas. If a modal dialog is waiting for an answer, calls
return `ui.blocked_by_dialog` and the agent can inspect and answer it with the `ui_dialog_*`
tools. Disable the endpoint with `PRINTBOT_MCP_DISABLE=1` or `mcp_server_enabled = false` in
the app config.

**Headless mode.** With no app running (or `--headless`) `print_bot-mcp` runs the wx-free core
itself: presets, load models, slice and export only (17 tools), never a printer.

    ./print_bot-mcp.sh [--headless | --gui] [--socket PATH] [--wait S] [--datadir DIR] [--tempdir DIR] [--log-level N]

- `--gui`: require the running app (exit 1 if there is none) instead of falling back.
- `--socket PATH`: endpoint to connect to (default as above). `--wait S`: wait up to S seconds
  for the app to come up.
- `--datadir` (headless): config dir with presets (default `$PRINTBOT_DATADIR`, else
  `${XDG_CONFIG_HOME:-~/.config}/PrintBot`). Read-only use; use a separate dir to keep it
  isolated from the GUI. `--resources`: the repo's `resources/` (the wrapper passes it).
- stdout is protocol only; logs go to stderr.

**Printer control is off by default.** Starting a print (`print_plate` with `dry_run = false`) and
pausing/resuming/stopping (`printer_control`) are refused (`policy.denied`) unless the user starts
the app with `PRINTBOT_MCP_ALLOW_PRINTER_CONTROL=1` or sets `mcp_allow_printer_control = true` in the
app config. An agent may enable this only with explicit human authorization to enable MCP control.
Enabling control is an additional requirement; it never approves a print.

Before preparing every approval, run the sidebar Sync Info action (`ui_open_dialog` with
`sync_info`) and complete Continue to sync filaments (`sync_filaments` can reopen that step). Verify
actual presets and native feeder labels such as HT-A, then re-slice if synchronization changed
settings. Opening a synchronization dialog alone is not success. The agent still presents the exact
project/plate/objects/copies, destination printer, filament mapping, estimated time and material
use, and asks before every print.

**The human authorization gate.** Every MCP print goes through one gate (`print_plate`, nothing else):
1. `print_plate {}` (dry run) returns the job summary and a `job_fingerprint` (SHA-256 over the
   project, plate, object instances, G-code file, selected presets, full and plate settings, and
   destination printer).
2. `print_plate {dry_run: false, fingerprint}` requires printer control, refuses a changed job
   (`approval.stale`), opens the Print dialog if needed, and freezes the job plus the dialog's actual
   destination, filament mapping and options into an immutable approval fingerprint.
3. print_bot shows its own native modal **Approve print** dialog with a read-only summary. Cancel is
   the default, Approve is disabled for 1.5 s and needs a real mouse press or key press on the button.
   The dialog is invisible to MCP: it is not listed or inspectable, and every MCP tool call returns
   `approval.pending` while it is open. Print/Send dialog buttons and fields cannot be clicked or set
   through MCP at all (`print.human_only`); `confirm` arguments are ignored.
4. After approval the gate rechecks control policy and the full fingerprint, then presses Send.
   The Print dialog runs the same check again immediately before the print job starts transport
   (after any warnings the user answers). Any difference aborts with nothing sent. The approval is
   single-use: it is consumed by the first send attempt, expires after 10 minutes, and every retry
   or repeat needs a new dialog.
5. Success returns `status: send_requested` (or a later observed state) and an `operation_id`, never
   "sent". `get_print_operation` reports the transfer (`send_requested`, `awaiting_user_confirmation`,
   `uploading`, `transferred`, `failed`, `not_sent`) and the printer's own report (`printing`, `paused`,
   `finished`, `printer_failed`, `no_matching_job_reported`). Anything not observed stays `unknown`.
   Never resend because an outcome is unknown.

The headless server has no print routes and never sends. For tests, `PRINTBOT_MCP_TEST_NO_SEND=1`
runs the whole gate but never presses Send.

Robustness: malformed JSON-RPC (wrong types for `id`, `method`, `params`, tool name, `arguments`,
`_meta`, progress tokens, bad batches, parse errors) gets a JSON-RPC error on the same connection;
exceptions are caught at the session, request and tool-worker boundaries, so neither the headless
bridge nor the GUI exits. `printer_camera_snapshot` runs ffmpeg directly with an argument vector
(no shell), a timeout (`timeout_s`, default 20 s) and error messages that never include the stream
URL or access code. `select_presets` is transactional: a failed selection restores the previous
printer, process, filaments and bed, and any retained change publishes a new revision and marks
slices outdated.

Live tools (58):
- General: `get_status`, `get_version`, `get_events`.
- Presets: `list_printer_presets`, `list_process_presets`, `list_filament_presets`,
  `get_selected_presets`, `select_presets` (printer, process, filament slots, bed type),
  `get_preset_settings`, `set_preset_settings`, `save_preset` (user preset),
  `discard_preset_changes`.
- Project: `new_project`, `open_project` (3MF with its settings), `save_project`, `load_model`,
  `undo`, `redo`.
- Objects (addressed by id): `list_objects`, `remove_object`, `clear_project`, `select_objects`,
  `transform_object` (position / move / rotate / scale / size), `set_object_settings`
  (per-object overrides), `set_object_filament`, `rename_object`, `clone_object`,
  `arrange_objects`, `orient_objects`.
- Plates (numbered from 1): `list_plates`, `add_plate`, `delete_plate`, `select_plate`,
  `set_plate_settings` (name, bed type, print sequence), `move_object_to_plate`.
- Slicing / export: `slice` (plate or all; waits; progress notifications), `get_slice_status`,
  `cancel_slice`, `export_gcode`, `export_sliced_file` (.gcode.3mf).
- Printers: `list_printers` (LAN discovery), `get_printer_status` (state, temperatures, AMS,
  HMS), `connect_printer` (access code), `disconnect_printer`, `printer_camera_snapshot`
  (needs ffmpeg; `timeout_s`).
- Print jobs (gated, see above): `print_plate` (dry run by default; `open_dialog` shows the
  Print dialog without sending), `get_print_operation` (outcome of a send), `printer_control` (pause / resume / stop; dry run by default).
- UI: `ui_get_state`, `ui_switch_tab`, `ui_set_camera`, `ui_screenshot` (canvas, window or
  dialog to PNG; window/dialog capture is X11 only), `ui_list_dialogs`, `ui_dialog_inspect`,
  `ui_dialog_click`, `ui_dialog_set`, `ui_close_dialog`, `ui_open_dialog`.

Headless tools: `get_status`, `get_version`, `list_printer_presets`, `list_process_presets`,
`list_filament_presets`, `get_selected_presets`, `select_presets`, `load_model`,
`list_objects`, `remove_object`, `clear_project`, `arrange_objects`, `slice`,
`get_slice_status`, `cancel_slice`, `export_gcode`, `get_events`.

Agent skill: [`skills/printbot/SKILL.md`](skills/printbot/SKILL.md) teaches an LLM agent to use these tools (workflows, safety rules, error codes); [`skills/printbot/tools.md`](skills/printbot/tools.md) is the full parameter reference generated from the schemas.

Register it with an MCP client (`mcp.json` / `.cursor/mcp.json` style; use the absolute path of
your checkout):

```json
{
  "mcpServers": {
    "print_bot": {
      "command": "/path/to/print_bot/print_bot-mcp.sh"
    }
  }
}
```

Code layout: `src/core/` is the wx-free core (event bus, single core thread, snapshots,
preset/project/slice services; the `printbot_no_gui_check` build step fails on any `wx/`,
`slic3r/GUI/` or `slic3r/Utils/` include in `src/core` or `src/mcp`). `src/mcp/` holds the MCP
protocol server, the headless tools and the socket transport (`McpSocket`). The live endpoint
and its tools are in `src/slic3r/GUI/PrintBotMcp.cpp` (socket host, main-thread marshalling,
agent notifications, policy) and `PrintBotMcpTools.cpp` (tools). The GUI uses the same event
bus: `BackgroundSlicingProcess` publishes slice milestones on it and the Plater turns them back
into its wx events via `CallAfter`.

### Home page "User Manual" section (deleted outright)
The Home tab's left-menu "User Manual" entry and everything behind it are gone: the in-app wiki
webview (`m_browserWiki`, `homepage3/wiki.html`, `js/wiki.js`, `css/wiki.css`), the
`WikiGuideBoard` tutorial cards in `home.html`, the wiki search / academy-list fetchers
(`WebViewPanel::get_wiki_search_result`, `get_academy_list`) and the `search_wiki`,
`get_academy_list`, `userguide_wiki_open` web-bridge commands in `GUI_App`. The Home tab now
shows only Library and Recently Opened. (Superseded: the whole web Home page is now native, see
"WebKit removed entirely" below.)

### Setup wizard deleted, native "Printers and filaments" dialog

The webview setup wizard (`GuideFrame`, `WebGuideDialog.cpp/.hpp`, `resources/web/guide/{0,1,11,21,22,22_remove,24,3,31,32,5,index.html,swiper,js/home.js}`)
and `UxProgramTermsDialog` (plus the Preferences "User Experience Improvement Program" checkbox) are removed
(copies in `temp/removed_t55u/`). The rest of `resources/web/` went with the WebKit removal (below). Help > Setup Wizard,
`GUI_App::ShowUserGuide()` and `ShowOnlyFilament()` are gone.

`GUI_App::run_wizard()` keeps its signature (all callers unchanged) but now opens
`PresetSelectionDialog` (src/slic3r/GUI/PresetSelectionDialog.*): a native wxNotebook with
- **Printers**: one row per installed-vendor FFF model, a checkbox per nozzle size → AppConfig vendor map (`set_vendors`).
- **Filaments**: search box, "only filaments for the ticked printers" filter, checklist of system filament
  families (alias); ticking a family writes all its system preset names to the `filaments` section.
- **Create custom filament...**: applies, then opens the native `CreateFilamentPresetDialog`.
OK applies like the wizard did (unsaved-changes check, `load_presets`, colour alignment, `export_selections`);
a newly ticked printer becomes the selected one. Multi-nozzle printers (H2D/H2D Pro/H2C/H2S/X2D) are listed and
nothing multi-extruder was removed.

Where to open it (Prepare tab sidebar):
- Printer section gear, or printer dropdown → "Select/Remove printers(system presets)" → Printers tab.
- Printer dropdown → "Create printer" → native CreatePrinterPresetDialog (unchanged).
- Filament settings gear ("Set filaments to use"), or a filament dropdown → "Add/Remove filaments" → Filaments tab.

First run (no config / only default printers): no wizard. `config_wizard_startup()` installs the BBL bundle,
enables **Bambu Lab P2S 0.4 nozzle**, clears the filaments section so `load_installed_filaments()` enables the
P2S default Bambu filaments, sets `firstguide/finish`, and selects P2S + PLA Basic.

Plater's create-filament flow no longer re-opens the wizard and uses the native CreateFilamentPresetDialog;
`on_modify_filament` (reached via EVT_MODIFY_FILAMENT from ParamsDialog) uses the native EditFilamentPresetDialog.

### Native "Purging volumes" dialog and nozzle table (webview `flush/` removed)

`WipingDialog` (src/slic3r/GUI/WipeTowerDialog.*) is now plain wxWidgets: a `wxGrid` (filament-colour
badges in the row/column headers, "from/to" corner, read-only diagonal), Re-calculate button, a
Left/Right (H2D-style) extruder selector shown only when `nozzle_diameter` has >1 entry, the multiplier
box and OK/Cancel. Behaviour follows the old `resources/web/flush/WipingDialog.html` one-to-one:
- shown value = round(raw × multiplier) clamped to [0, max]; typed value → raw = round(shown / multiplier);
- multiplier is clamped to [0, 3] while typing (out of range snaps to the bound), one per extruder;
- Re-calculate replaces only the current extruder's matrix with `CalcFlushingVolumes(extruder)`;
- orange = differs from the auto value, red = outside the suggested [min, max] range (range line turns red);
- OK clamps raw to max/multiplier, expands the physical-filament sub-matrix back to the full matrix and
  writes `flush_volumes_matrix` + `flush_multiplier` (or `flush_multiplier_fast` in fast prime mode) exactly
  as before (`open_flushing_dialog` / `is_flush_config_modified` unchanged).
Cell edits commit on Enter / leaving the cell. Colours have dark variants via `GUI_App::dark_mode()`.

`NozzleListTable` (Widgets/MultiNozzleSync.*, used by `MultiNozzleSyncDialog`) is a painted wxPanel with the
same layout as the old `flush/NozzleListTable.html` (Nozzle Selection | Available Nozzles Left/Right, radio per
diameter row, EVT_NOZZLE_SELECTED on change). The real dialog needs a connected multi-nozzle printer, so for
UI testing `PRINTBOT_UITEST_NOZZLE_TABLE=1 ./build/src/bambu-studio` opens a sample table at startup
(`printbot_test_nozzle_list_table`, hook in `GUI_App` startup).

`resources/web/flush/` is deleted (copy in `temp/removed_t57u/`). `resources/flush/` (flush-volume predictor
data used by libslic3r `FlushVolPredictor`) is unrelated and stays.

Note: an app started before this change still has the old web dialog compiled in; with `flush/` deleted it
shows an empty window ("WipingDialog.html not exist" in the log). Restart print_bot after rebuilding.

### WebKit removed entirely (no wxWebView, no WebKitGTK/libsoup/JavaScriptCore)

`ldd build/src/bambu-studio | grep -Ei 'webkit|javascriptcore|soup'` is empty and `src/` has no wxWebView use.
Build: `webview` dropped from `find_package(wxWidgets ...)` in `src/CMakeLists.txt`; the webkit2gtk
`pkg_check_modules` / `BBL_WEBKITGTK` defines and all web sources dropped from `src/slic3r/CMakeLists.txt`.
`print_bot.sh` and `BambuStudio.cpp` no longer set `WEBKIT_DISABLE_COMPOSITING_MODE`.

Deleted (copies in `temp/removed_t61u/`):
- `WebViewDialog.*` (web Home page `WebViewPanel`, `homepage3/`), `Widgets/WebView{,TraceLogger,Watcher}.*`.
- Cloud web dialogs: `WebUserLoginDialog.*`, `ModelMall.*`, `WebDownPluginDlg.*`, `PrivacyUpdateDialog.*`,
  `MarkdownTip.*` (sidebar tooltip popups), `PrinterWebView.*` (non-Bambu printer web UI tab), the Helio and
  UpdateVersionDialog webviews (the native parts of `ReleaseNote.*`/`HelioReleaseNote.*` stay).
- `CreateFilamentWebDialog.*`, `EditFilamentWebDialog.*` → native `CreateFilamentPresetDialog` / `EditFilamentPresetDialog`.
- `fila_manager/wgtFilaManagerPanel.*` (Filament Manager tab). The non-web fila_manager store/sync code stays
  (StatusPanel, AMS settings and FilamentSelectDialog use it).
- `DeviceWeb/` (web Device page + view models) and `DeviceTab/AmsControl/` (web AMS panel, `AmsControlWebEnable.hpp`).
  The Device tab always uses the native `StatusPanel` + `AMSControl`.
- `resources/web/` (all of it, 54 MB).
- macOS-only `WKWebView_*` helpers in `Utils/MacDarkMode.{mm,hpp}`; app-config keys `enable_webview_devtools`,
  `webview_auto_fill` are erased on load.

Home tab: `HomePanel` (src/slic3r/GUI/HomePanel.*) — New Project / Open Project buttons and a grid of recent
projects (3MF thumbnail, file name, modified time; click opens it, right-click to remove, "Clear list").
It reads `MainFrame::get_recent_project_list()`; `MainFrame::m_webview` keeps its name (now `HomePanel*`) so the
existing `SendRecentList()` refresh calls are unchanged. Opening a missing recent file asks whether to drop it.

### t63u prune: everything outside the current feature set removed

Kept: Prepare/Preview slicer and all Bambu presets (incl. multi-nozzle H2D/H2C), native Home page, native
Printers-and-filaments dialog, 3MF/STL/STEP/OBJ import/export, LAN discover/connect/upload/print (OBN plugin),
the Device tab (status, controls, native AMS, camera via libBambuSource/GStreamer), native purging-volumes dialog
and nozzle table, Preferences and translations. libslic3r is untouched apart from build fixes.

Removed (copies mirrored under `temp/removed_t63u/`, pre-edit originals in `temp/backup_t63u/`):
- Dead GUI/Utils files: `AmsWidgets`, `Auxiliary*`, `GUI_AuxiliaryList`, `BBLStatusBar`, `BBLStatusBarBind`,
  `ProgressStatusBar`, `BetaVersionDialog`, `GuiTextShape`, `MonitorBasePanel`, `MonitorPage`, `SliceInfoPanel`,
  `Widgets/ErrorMsgStaticText`, `Gizmos/GLGizmoFaceDetector`, `Utils/HexFile`, `Utils/Profile*`, `minilzo` +
  `minilzo_extension`, `wxMediaCtrl2.cpp` (Linux/Windows; `wxMediaCtrl2.mm/.h` stay for macOS).
- Cloud: `HttpServer`, `UserManager`, `PublishDialog` and the publish menu/topbar button, `VersionPolicy*`
  (slice version policy), `DownloadProgressDialog`, `Jobs/UpgradeNetworkJob`, `WebUpdatePlugin`,
  `NetworkTestDialog`, `SendSystemInfoDialog`, `UpdatePluginDialog`/`UpdateVersionDialog` (in `ReleaseNote.*`),
  the online parts of `PresetUpdater` (version/resource/plugin/printer-config download, OTA parsing; bundled
  profile install + local config-update logic stay), login/privacy/version-check events and GUI_App helpers.
- Binding: `BindDialog`, `Jobs/BindJob` (the IP + access-code `InputIpAddressDialog` stays).
- Helio: `Utils/Helio*`, `HelioReleaseNote`, `HelioHistoryDialog`, its tests, `data/helio_hints.ini`, images.
- Cloud multi-machine: `MultiMachine*`, `MultiPrintJob`, `MultiSendMachineModel`, `MultiTask*`,
  `SendMultiMachinePage`, `TaskManager`.
- Go Live / virtual camera: streaming code in `MediaPlayCtrl`/`CameraPopup`/`StatusPanel`.
- MainFrame tab ids `tpFilamentManager`, `tpWebDevice`, `toDebugTool`; Preferences Filament Manager checkbox.
- Resources: non-Bambu vendor profiles (Anker, Anycubic, Creality, Elegoo, Geeetech, Prusa, Qidi, Tronxy,
  Vivedino, Voron, Voxelab), `calib/`, `dailytip/`, web-only `tooltip/` files (`tooltip/images` stays for
  ParamTooltip), 74 unreferenced images (calibration pages, Helio, vcamera, mall, publish, account, auxiliary).
- Deps/build: `deps/WebView2`, `deps/OpenCSG` (+ its `deps/CMakeLists.txt` entry), the WebView2Loader copy in the
  root CMakeLists, the wxWidgets `_wx_edge` block; wxWidgets is now configured with `-DwxUSE_WEBVIEW=OFF`
  (only takes effect on a deps rebuild).
- `legacy_python/` (the old Python GTK4 app).

Kept but questionable (still compiled, not reachable or not testable here):
- Non-Bambu print-host code (`PrintHost`, OctoPrint/Duet/Repetier/MKS/FlashAir/AstroBox, `PhysicalPrinterDialog`,
  Bonjour) for custom printers.
- `fila_manager` store/sync (runtime-disabled), `link_to_network_check` stub, `is_user_login`, the NetworkAgent
  cloud ABI wrappers (plugin ABI), `VersionInfo`.
- MakerWorld rating / `ScoreDialog` / market-scoring code inside StatusPanel/DeviceManager (Device tab, risky).
- The Helio G-code viewer pieces in `GCodeViewer`, `CalibrationWizardPage.*` (shared widgets), `dark_mode.cpp`
  (Windows), the macOS STUDIO-18472 idle workaround.
- `resources/hms` (150 MB) — Device tab HMS error texts.

### t75u prune (round 2) + t84u dark-mode fix

Kept on purpose (automation infrastructure for the MCP server): the CLI in
`src/BambuStudio.cpp`, `xs/`, `t/`, `lib/`, `Build.PL`, `PRINTBOT_UITEST` hooks, action/command dispatch and the
wx event plumbing; `resources/hms` (Device tab HMS texts); macOS/Windows sources (`*.mm`, `dark_mode.cpp`, ...).

Removed (copies under `temp/removed_t75u/`, pre-edit originals in `temp/backup_t75u/`):
- Root clutter: Docker/Containerfile files, `BuildFedora.sh`, sublime project, `localazy.json`, `BambuStudio.mo`,
  `diff_summary.txt`, release notes, `sandboxes/`, `bbs_test_tools/` (+ their CMake options/blocks, dead MSVC block).
- Non-Bambu print hosts: `Utils/{PrintHost,OctoPrint,Duet,Repetier,FlashAir,AstroBox,MKS,Bonjour,TCPConsole}`,
  `GUI/{BonjourDialog,PhysicalPrinterDialog,PrintHostDialogs}` and the upload-queue/Ctrl+J/legacy send paths.
- `GUI/fila_manager/` (store/sync), MakerWorld rating (`ScoreDialog`, DeviceManager rating/model-mall bits),
  `CalibrationWizardPage.*`, `CalibrateFilamentComboBox`; 44 dead cloud wrappers and 29 unused ABI structs in
  `NetworkAgent`.
- SLA GUI: `GLGizmoSlaSupports`, `GLGizmoHollow`, `Jobs/SLAImportJob` (libslic3r SLA core stays: CLI uses it).
- Never-linked code (found with a `--gc-sections` + map-file scratch build, `temp/t75u/notlinked.txt`): legacy
  `ConfigWizard.cpp`/`ConfigWizard_private.hpp` (`ConfigWizard.hpp` is now just the RunReason/StartPage enums;
  `run_wizard` opens the native Printers-and-filaments dialog), `Config/Snapshot.*`, `Config/Version.cpp`,
  `SysInfoDialog`, `wxinit.h`, libslic3r `JumpPointSearch`, `PolygonTrimmer`, `SLA/bicubic.h`,
  `SLA/RasterToPolygons`, four `CSGMesh` headers, libnest2d `rotfinder`/nlopt `simplex`.
- `bambu_networking`: `research/`, plugin_runner/mock_harness, credential fetch script, cloud tests
  (cloud_print/filament_body/device_region/oss_sign), RE/frida/TUTK/FTP-proxy tools.
- 41 unreferenced images in `resources/images` (dynamic-name families such as `Nozzle_*`, `printer_preview_*`,
  `ams_humidity_*` were kept).

Dark mode, Printers-and-filaments dialog (`PresetSelectionDialog`, t84u): text was dark-on-dark and the filament
checklist white in the app's dark mode, because native GTK widgets kept the system theme's colours. The
`wxNotebook` was replaced by a themed `::Button` tab strip over a `wxSimplebook`, the `wxButton`s by `::Button`s
with the usual StateColor recipe, and every native child (labels, checkboxes, checklist, search field, panels)
gets explicit palette colours. The dialog follows the same dark switch as the rest of the themed UI (StateColor's
flag) instead of re-reading `dark_color_mode`, which the GTK sys-colour handler can rewrite at runtime.
Screenshots: `temp/t84u/` (dark with GTK light theme, dark with GTK dark theme, light).

Testing note: OBN's `appcert_fetch` is on by default even in the LAN-only build; a fresh config fetches the
shared slicer app cert from api.bambulab.com and the plugin then sends `app_cert_install` to a LAN printer it
connects to. Test configs must pre-seed `appcert_fetch = 0` in `<config>/PrintBot/obn.conf`.
