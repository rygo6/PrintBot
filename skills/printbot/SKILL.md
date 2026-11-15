---
name: printbot
description: Drive print_bot (the LAN-only Bambu Lab slicer, a Bambu Studio fork) through its MCP server. Use when asked to load models, pick printer/process/filament presets, arrange or transform objects, manage plates, slice, export G-code or .gcode.3mf, check a printer's status, operate the print_bot window (tabs, camera, screenshots, dialogs), or prepare/send a print, on print_bot or a Bambu printer such as the P2S over the LAN.
---

# print_bot over MCP

print_bot is a LAN-only Bambu Lab slicer. Its MCP server, `print_bot-mcp`, has two modes:

- **Live** (the default when the print_bot app is running). The server is a thin stdio bridge to
  the app's private Unix socket. Every tool runs inside the app, on the same code paths as the user's
  clicks, so the user **sees everything you do** in the window: models appear, presets switch,
  the preview opens after slicing, and an "Agent: ..." notification names the current action.
  It has 58 tools: presets, project, objects, plates, slicing, printers, print jobs and UI.
- **Headless** (when no app is running, or with `--headless`). The wx-free core runs in its own
  process with nothing on screen. It has 17 tools: presets, load, arrange, slice and export.
  It has no plates, printers, print jobs or UI.

The full parameter reference, generated from the real schemas, is in [tools.md](tools.md).

## Configure the server

Use the wrapper script in the checkout. It passes `--resources` for you.

```json
{
  "mcpServers": {
    "print_bot": { "command": "/path/to/print_bot/print_bot-mcp.sh" }
  }
}
```

Flags (append them to `args`):

| flag | meaning |
|---|---|
| *(none)* | Live if the app is running, otherwise headless (it prints a note on stderr). |
| `--gui` | Require the running app; exit 1 if there is none. Use this when the user wants to watch. |
| `--headless` | Always run the in-process server, even when the app is open. |
| `--socket PATH` | Connect to this app socket. Default: `$PRINTBOT_MCP_SOCKET`, else `$XDG_RUNTIME_DIR/print_bot/mcp.sock`, else `/tmp/print_bot-<uid>/mcp.sock`. |
| `--wait S` | Wait up to S seconds for the app to come up (use it right after launching `print_bot.sh`). |
| `--datadir DIR` | Headless only: the preset/config dir (default `$PRINTBOT_DATADIR`, else `${XDG_CONFIG_HOME:-~/.config}/PrintBot`). Pass a separate dir to stay isolated from the app. |
| `--tempdir DIR`, `--log-level 0-5` | Headless scratch dir; log verbosity on stderr. |

The mode is fixed when the server starts. If the app was opened or closed afterwards, restart the
MCP server to switch modes. stdout carries only the protocol.

## Always start with `get_status`

Call `get_status` first and read:

- `mode`: `"live"` or `"headless"`. Headless has no plates, printers or `ui_*` tools.
- `presets`: the printer, process, filament slots, bed type, and whether each has unsaved edits.
- `project` (name, path, dirty, objects, plates) and `current_plate` with its slice summary.
- `ui` (live): `tab`, `view`, `selected_object_ids`, and **`dialogs`**. If a dialog is listed,
  handle it before anything else (see Dialogs below).
- `printer_control_allowed`: whether the user enabled printer control (normally `false`).
- `selected_printer` (live), present when a printer is selected.

Never assume the window is empty: the user may have their own project open. If `project.dirty`
is true, ask the user before `new_project` / `open_project` with `discard_unsaved: true`.

## Addressing

- **Objects** are addressed by `id` (the `id` field from `list_objects` / `load_model.added`),
  not by list position. Ids stay stable while the object exists.
- **Plates** use numbers starting at 1, as labelled in the app. Omitting `plate` means the current plate.
- **Filament slots** use numbers starting at 1.
- **Presets** take the exact name, e.g. `"Bambu Lab P2S 0.4 nozzle"`, `"0.20mm Standard @BBL P2S"`,
  `"Bambu PLA Basic @BBL P2S"`. A unique case-insensitive substring is also accepted. Use the
  `list_*_presets` tools with `filter` to find names.

## Workflows

### 1. Model, presets, slice, export (works in both modes)

1. `get_status`
2. `list_printer_presets {filter: "P2S"}`, then `select_presets {printer: "<printer preset>"}`.
   Select the printer first: it changes which process and filament presets are compatible.
3. `list_process_presets {filter: "0.20mm"}` and `list_filament_presets {filter: "PLA"}`, then
   `select_presets {process: "...", filaments: ["slot 1 preset", "slot 2 preset"]}`.
   `filaments` sets the slots in order and adds or removes slots to match its length.
   `bed_type` takes one of `get_selected_presets.available_bed_types`.
4. `load_model {path: "/abs/path/model.stl"}`. Live mode also accepts `paths: [...]`. Use
   `append: false` to start fresh, and `load_settings: false` to load a 3MF's geometry without its presets.
   Keep the returned `added[].id`.
5. `arrange_objects`, or place objects yourself with `transform_object`.
6. `slice`. It waits by default (`timeout_s` 900) and returns `print_time`, `filament_used_g/m`,
   per-slot filament, `layer_count` and `warnings`. Send `_meta.progressToken` to get
   `notifications/progress`. Live mode switches to the Preview tab.
7. `export_gcode {path: "/abs/out/name.gcode"}`. In live mode the plate **must already be sliced**.
   For the printer-ready package use `export_sliced_file {path: "/abs/out/name.gcode.3mf"}`
   (add `all_plates: true` for every sliced plate). `save_project {path: "/abs/out/name.3mf"}`
   saves the editable project.

Output paths must be absolute and their directory must exist. Missing extensions are added.

### 2. Placing and transforming objects (live)

`transform_object {id, ...}` applies its parts in the order rotate, scale/size, move:

- `rotate_deg: [rx, ry, rz]` is a *relative* rotation around the world axes.
- `scale` is an *absolute* factor: a number for uniform scaling, or `[sx, sy, sz]`, along the
  object's own axes, with 1 = the original size.
- `size_mm: [x, y, z]` is a target size along the object's own axes.
- `position: [x, y]` or `[x, y, z]` is the absolute position of the bounding-box center in bed mm.
  `move_by` is a relative move.

The response's `size_mm`/`min`/`max` are the *world* bounding box, so after a rotation they differ
from the `size_mm` you asked for. Check `outside_print_volume` in the response or in `list_objects`.

Other object tools:
- `set_object_settings {id, values: {"sparse_infill_density": "40%", "wall_loops": "4"}}` adds per-object overrides; a `null` value removes one.
- `set_object_filament {id, filament: 2}`
- `rename_object`
- `clone_object {id, copies}`
- `remove_object {id | ids}`
- `orient_objects {ids?}`
- `select_objects {ids | all}` (UI selection only)
- `undo` / `redo`

### 3. Several plates (live)

1. `add_plate`. The new plate becomes the current one.
2. `load_model`. New objects land on the current plate. Or move an existing object with
   `move_object_to_plate {id, plate: 2}`.
3. `set_plate_settings {plate: 2, name: "...", bed_type: "Textured PEI Plate" | "default",
   print_sequence: "default" | "by layer" | "by object"}`
4. `list_plates` shows each plate's `object_ids` and slice summary.
5. `slice {plate: 2}` or `slice {all_plates: true}`; `select_plate {plate}` switches the view.
6. `delete_plate {plate}` also deletes the objects on it. The last plate can't be deleted.

**Important:** `arrange_objects` works like the app's Arrange button. Objects that don't fit
spill onto further plates, and **empty plates are removed**. To keep an empty plate for later,
arrange first, then `add_plate`.

### 4. Editing and saving presets (live)

1. `get_preset_settings {type: "process", keys: ["layer_height", "sparse_infill_density"]}`.
   With no `keys` it returns the edited options plus common ones; `all: true` returns everything (large).
2. `set_preset_settings {type: "process", values: {"layer_height": "0.16", "sparse_infill_density": "20%"}}`.
   Values use the config text format, so send strings: percentages as `"20%"`, booleans as `"1"`/`"0"`.
   The change appears as an unsaved edit in the settings panel. Filament edits apply to
   `get_selected_presets.edited_filament`.
3. `save_preset {type: "process", name: "My 0.16 @P2S"}` saves a user preset and selects it.
   System preset names are refused. Or call `discard_preset_changes {type}` to drop the edits.
4. Re-slice after any preset change.

### 5. Driving the UI (live)

- `ui_switch_tab {tab: "home" | "prepare" | "preview" | "device"}`
- `ui_set_camera {view: "iso" | "top" | "bottom" | "front" | "rear" | "left" | "right", zoom: "bed" | "objects"}`
- `ui_screenshot {path: "/abs/shot.png", target: "canvas" | "window" | "dialog", dialog?}` saves a PNG.
  `canvas` (the default) is the 3D/preview view. `window` and `dialog` capture what is on screen,
  including dialogs on top. Look at the PNG to verify your work, especially after transforms,
  arranging and slicing.
- `ui_open_dialog {dialog: "preferences" | "printers" | "filaments" | "printer_settings" |
  "process_settings" | "filament_settings" | "shortcuts" | "print" | "send" | "sync_info" | "sync_filaments"}`. `print` and `send`
  only open the dialog and send nothing; they need a sliced plate.
- `ui_get_state` gives the tab, view, selection and open dialogs.

### 6. Dialogs and `ui.blocked_by_dialog` (live)

The app sometimes asks the user something, for example **"Restore project?"** after a crash, an
unsaved-changes prompt, or a confirmation. While a modal dialog is open, a tool whose work waits on
it returns **`ui.blocked_by_dialog`**. The action usually finishes once the dialog is answered, so
don't blindly repeat it.

1. `ui_list_dialogs` returns `index`, `title`, `class`, `modal`, and `topmost`.
2. `ui_dialog_inspect {dialog: <index or title>}` lists the widgets (text, fields, checkboxes,
   choices, buttons) with numbers.
3. If the right answer isn't obvious or could lose data ("Restore project?", "Don't save",
   overwrite prompts), **ask the user**. Otherwise:
   - `ui_dialog_click {dialog, button: "OK"}` or `{dialog, widget: n}`. A checkbox widget toggles.
   - `ui_dialog_set {dialog, widget, value}` for text fields, checkboxes, radio buttons and choices.
   - `ui_close_dialog {dialog}` acts like the window's close button (Cancel).
4. Call `get_status` again to confirm `ui.dialogs` is empty. Then check the state and retry only
   if the action didn't take effect.

`dialog` defaults to the topmost dialog. Titles match case-insensitively, including by substring.

### 7. Printers (live; reading is safe)

- `list_printers` shows LAN-discovered printers: `id` (serial), `name`, `model`, `online`,
  `connected`, `busy`, `selected`.
- `get_printer_status {printer?}` shows the print state, progress, layer, remaining minutes,
  nozzle/bed/chamber temperatures, AMS trays (including native `label` such as `HT-A` or `A1`),
  HMS errors, and `can_pause`/`can_resume`. Use the label in user-facing approval summaries.
  It is read-only.
- `connect_printer {printer, access_code?, wait_s?}` selects and connects to a printer, and opens
  the Device tab. The 8-character access code is needed only once; ask the user for it and never
  invent or log it. Connecting starts nothing on the printer.
- `disconnect_printer`
- `printer_camera_snapshot {printer?, path, timeout_s?}` saves one camera frame. It needs ffmpeg and LAN
  liveview; otherwise it returns `camera.unavailable` (or `camera.timeout`).

`printer` accepts the serial, the name, or the IP from `list_printers`.

### 8. Sending a print (fresh approval for every print)

1. Before preparing EVERY print approval, run `ui_open_dialog {dialog: "sync_info"}`. This is
   the sidebar **Sync Info** action for nozzle and AMS information. Inspect its dialog and
   choose **Continue to sync filaments**. If that temporary prompt has disappeared, run
   `ui_open_dialog {dialog: "sync_filaments"}` instead. Inspect the synchronization choices,
   complete **Synchronize now**, and verify the resulting presets, source labels and colors
   with `get_selected_presets` and `get_printer_status`. Opening a dialog alone is not a
   completed sync. Preserve intentional project overrides and handle any unsaved-edit prompt.
   Then prepare and slice the exact plate. Re-slice after sync changes settings; any earlier
   approval must be renewed after such changes. Read `get_printer_status` and run `print_plate {}`
   (dry run). Its `job`, `printer`, `presets` and slice summary describe the proposed print.
   Open the Print dialog with `print_plate {open_dialog: true}`, then use `ui_dialog_inspect`
   to verify its actual `printer`, `filament_mapping_valid`, `filament_mapping` and
   `filament_mapping_info`. Mapping entries follow project filament order; AMS and slot IDs
   are the printer's raw IDs. Compare them with `get_printer_status` before presenting approval.
2. Present the project/file, plate, object names and copy counts, destination printer name/model
   (plus a distinguishing identifier if needed), material and filament mapping, estimated time
   and material use. Ask: "Start this specific job on this specific printer?"
3. **Wait for the human user's explicit affirmative answer to that summary.** This is required
   for EVERY print, even when the user already asked to print in general or enabled MCP control.
   An earlier print's approval never authorizes another print.
4. Call `print_plate {dry_run: false, fingerprint: <job_fingerprint of the dry run you presented>}`.
   print_bot opens its own native **Approve print** dialog; only the user's click there approves.
   Tell the user to review and approve it in print_bot. While it is open, every tool call returns
   `approval.pending`; you cannot see, inspect or click it, and Print/Send dialog buttons and fields
   are never clickable or settable over MCP (`print.human_only`). Do not try other routes.
5. If the job, settings, material, mapping or destination changed, the call returns
   `approval.stale` and nothing is sent. Present the new summary, get fresh agreement, and start again
   from a new dry run. Approval is single-use; every retry or repeat needs a new dialog.
6. Success returns `status: send_requested` and an `operation_id`; it does not mean the printer
   accepted the job. Poll `get_print_operation {operation_id}` and report only what it shows
   (`uploading`, `transferred_awaiting_printer`, `printing`, `failed`, `not_sent`, ...). Report
   `unknown` as unknown and never resend automatically.

`printer_control {action: "pause" | "resume" | "stop", dry_run, confirm}` still requires
an explicit user request for that particular action. Enabling control authorizes no job or action.

## Safety rules (non-negotiable)

- **Never start, pause, resume, or stop a print, and never move, home, or heat anything, unless the
  user explicitly asked for that specific action in this conversation.** "Slice this" is not "print this".
- Printer control is **off by default**. `print_plate {dry_run:false}` and `printer_control
  {dry_run:false}` return **`policy.denied`** until the user starts print_bot with
  `PRINTBOT_MCP_ALLOW_PRINTER_CONTROL=1` or sets `mcp_allow_printer_control = true` in the app config.
  An agent may enable this only when the human explicitly authorizes enabling MCP printer control.
  That authorization is separate from, and does not replace, fresh approval for every print.
- Even with control enabled: finish the dry run, present the exact job and destination printer
  with material, mapping, time and usage, and obtain fresh explicit approval for each print.
  Never reuse approval or infer it from silence. The native approval dialog is the user's alone.
- Before printing, check `get_printer_status`: not `busy`, the right filament in the AMS, and no HMS errors.
- Don't discard the user's unsaved project or preset edits without asking.
- Don't paste access codes, serials, or IPs into files or messages unless needed.

## Errors

Failed calls return `isError: true` with `{"error": {"code", "message"}}`. The message says what to do.

| code | meaning / what to do |
|---|---|
| `invalid_argument` | Bad or missing parameter, unknown plate number, or relative path. Fix the call. |
| `ui.blocked_by_dialog` | A modal dialog is waiting. Handle it (workflow 6). |
| `ui.timeout`, `app.not_ready`, `app.exiting` | The app is busy, starting up, or closing. Wait and call `get_status`. |
| `policy.denied` | Printer control is off. Obtain explicit user authorization before enabling it. |
| `print.fingerprint_required` | Run the dry run, present its summary, then pass its `job_fingerprint`. |
| `approval.pending` | print_bot's approval dialog is open for the user. Wait; do not retry other tools in a loop. |
| `approval.stale` | The job changed since it was summarized or approved. Nothing was sent. Start again from a dry run. |
| `print.not_approved`, `approval.timeout` | The user cancelled or did not answer. Nothing was sent. Do not ask again unprompted. |
| `print.human_only` | Print/Send dialog controls are for the user only. Use `print_plate`. |
| `print.unknown`, `print.no_operation` | The outcome is not known. Check `get_print_operation` and printer status; never resend automatically. |
| `printer.not_confirmed` | Set `confirm: true` only after an explicit request for this specific printer action. |
| `print.not_sliced`, `export.not_sliced` | Slice first. |
| `print.not_ready`, `print.blocked`, `print.no_dialog`, `print.no_button` | The preflight failed or the Print dialog refused. Read `issues` or the dialog. |
| `printer.not_found`, `printer.none_selected`, `printer.not_connected`, `printer.no_access_code`, `printer.not_possible`, `printer.unavailable` | Use `list_printers` / `connect_printer`, or the action isn't possible in the printer's current state. |
| `preset.not_found`, `preset.unknown_option`, `preset.bad_value`, `preset.system`, `preset.filament_slots` | Check names with the `list_*` tools and option keys with `get_preset_settings`. |
| `object.not_found`, `object.unknown_option`, `object.bad_value` | Re-read `list_objects`; per-object settings must be process options. |
| `model.load_failed` | Nothing loaded (bad or unsupported file; see the app's notifications). |
| `slice.empty`, `slice.invalid_plate`, `slice.error`, `slice.cancelled`, `slice.timeout` | No objects, an object off the bed (`outside_print_volume`), or a slicing error. `slice.timeout` means the slice is still running; poll `get_slice_status`. |
| `export.failed`, `export.timeout`, `project.save_failed`, `project.no_path`, `project.cancelled` | File or IO problem. Check the path. |
| `plate.limit`, `plate.last`, `arrange.empty`, `orient.empty`, `undo.empty`, `redo.empty` | Nothing to do. |
| `ui.no_dialog`, `ui.dialog_not_found`, `ui.no_widget`, `ui.disabled`, `ui.not_editable`, `ui.bad_value` | Re-inspect the dialog. |
| `ui.no_canvas`, `ui.screenshot_failed`, `ui.screenshot_timeout`, `ui.unsupported` | The view isn't shown, or the platform doesn't support it (see tips). |
| `camera.unavailable`, `camera.failed`, `camera.timeout` | ffmpeg is missing, liveview is off, or the stream failed. |
| headless: `file.*`, `presets.*`, `preset.incompatible`, `project.empty`, `slice.outdated`, `slice.not_running`, `core.*` | File not found or unsupported, presets not loaded, an incompatible preset (`allow_incompatible`), or nothing to slice or export. |

## Tips

- `arrange_objects` removes empty plates and spills overflow onto new plates (see workflow 3).
- `clone_object` makes **separate objects**, each with its own id. Bambu Studio has no
  multi-instance copies; the old `set_instance_count` does not exist.
- `ui_screenshot` with `target: "window"` or `"dialog"` works on **X11 only** (Linux;
  `print_bot.sh` runs the app on X11/XWayland). Elsewhere it returns `ui.unsupported`; use
  `target: "canvas"`, which works wherever the window is rendering.
- `get_events {since}` polls core events (`slice.process_completed`, `slice.export_finished`, ...);
  keep `last_seq` between calls.
- `cancel_slice` stops a running slice; `clear_project` deletes all objects but keeps plates and settings; `get_version` reports the core and slicer versions.
- In live mode, the `slice` result and `get_slice_status` describe what the user sees in the Preview tab.
- `load_model` with a 3MF also loads its presets (`load_settings: true` by default) and may
  replace the current presets. Check `get_selected_presets` afterwards.
- After changing presets or object settings, the plate needs re-slicing (`sliced: false`).
- Headless tools differ slightly (`export_gcode` can `slice_if_needed`; `load_model` has `arrange`).
  See the end of [tools.md](tools.md).
