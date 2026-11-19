# print_bot MCP tool reference

Generated from the server's own `tools/list` schemas (live app: 58 tools; headless: 17).
**H** = also available in headless mode. **RO** = read-only (no change to the project, presets or printer).
Every call returns `structuredContent`; on failure `isError: true` and `{"error": {"code", "message"}}`.

## General

### `get_status`  (RO, H)

Live app state: project (name, path, dirty, objects, plates), selected presets, current plate with slice summary, UI state (tab, view, selection, open dialogs), selected printer, and whether printer control is allowed.

No parameters.

### `get_version`  (RO, H)

Versions of the print_bot core API and the slicing engine.

No parameters.

### `get_events`  (RO, H)

Core state-change events after a sequence number (slice.process_completed, slice.export_finished, ...). Poll with since = last_seq.

| param | type | required | description |
|---|---|---|---|
| `limit` | integer |  | Max events (default 100). |
| `since` | integer |  | Return events with seq > since (default 0). |

## Presets

### `list_printer_presets`  (RO, H)

Printer presets (one per printer model and nozzle).

| param | type | required | description |
|---|---|---|---|
| `filter` | string |  | Case-insensitive substring of the preset name. |
| `limit` | integer |  | Max presets returned (default 300). |
| `visible_only` | boolean |  | Only presets enabled in 'Printers and filaments'. Default false. |

### `list_process_presets`  (RO, H)

Process (quality) presets, by default only those compatible with the printer.

| param | type | required | description |
|---|---|---|---|
| `compatible_only` | boolean |  | Only presets compatible with the selected printer. Default true. |
| `filter` | string |  | Case-insensitive substring of the preset name. |
| `limit` | integer |  | Max presets returned (default 300). |
| `visible_only` | boolean |  | Only presets enabled in 'Printers and filaments'. Default false. |

### `list_filament_presets`  (RO, H)

Filament presets, by default only those compatible with the printer.

| param | type | required | description |
|---|---|---|---|
| `compatible_only` | boolean |  | Only presets compatible with the selected printer. Default true. |
| `filter` | string |  | Case-insensitive substring of the preset name. |
| `limit` | integer |  | Max presets returned (default 300). |
| `visible_only` | boolean |  | Only presets enabled in 'Printers and filaments'. Default false. |

### `get_selected_presets`  (RO, H)

Selected printer, process, filament slots and bed type, with unsaved-changes flags and the available bed types.

No parameters.

### `select_presets`  (H)

Select presets in the sidebar, exactly like choosing them in the combo boxes (the printer first). 'filaments' sets the filament slots in order and adds/removes slots to match its length. If presets have unsaved changes the app may ask what to do (ui.blocked_by_dialog).

| param | type | required | description |
|---|---|---|---|
| `bed_type` | string |  | Bed type, e.g. 'Textured PEI Plate' (see get_selected_presets). |
| `filaments` | array of string |  | Filament preset names, slot 1 first. |
| `printer` | string |  | Printer preset name, e.g. 'Bambu Lab P2S 0.4 nozzle'. |
| `process` | string |  | Process preset name, e.g. '0.20mm Standard @BBL P2S'. |

### `get_preset_settings`  (RO)

Read options of the selected (edited) printer, process or filament preset, including unsaved edits. Give 'keys' to read specific options (e.g. layer_height, sparse_infill_density, wall_loops, nozzle_temperature); without keys returns the options with unsaved edits plus a few common ones.

| param | type | required | description |
|---|---|---|---|
| `all` | boolean |  | Return every option (large). Default false. |
| `keys` | array of string |  | Option keys to read. |
| `type` | `printer` \| `process` \| `filament` | yes | Preset type. |

### `set_preset_settings`

Edit options of the selected printer, process or filament preset, like typing into the settings panel (the change shows as an unsaved edit and the plate needs re-slicing). Values use the config text format, e.g. {"layer_height": "0.16", "sparse_infill_density": "25%", "wall_loops": "3"}. Filament edits apply to the filament shown in the filament settings (get_selected_presets.edited_filament). Use save_preset to keep the changes as a user preset.

| param | type | required | description |
|---|---|---|---|
| `type` | `printer` \| `process` \| `filament` | yes | Preset type. |
| `values` | object | yes | Option key -> value (string or number). |

### `save_preset`

Save the selected printer/process/filament preset with its edits as a user preset named 'name' (like the Save button; an existing user preset with that name is overwritten). The new preset becomes selected.

| param | type | required | description |
|---|---|---|---|
| `name` | string | yes | User preset name. |
| `type` | `printer` \| `process` \| `filament` | yes | Preset type. |

### `discard_preset_changes`

Drop unsaved edits of the selected printer/process/filament preset.

| param | type | required | description |
|---|---|---|---|
| `type` | `printer` \| `process` \| `filament` | yes | Preset type. |

## Project

### `new_project`

Start a new empty project.

| param | type | required | description |
|---|---|---|---|
| `discard_unsaved` | boolean |  | Don't ask about unsaved changes. Default false (the app may ask: ui.blocked_by_dialog). |

### `open_project`

Open a .3mf project like File > Open project: its plates, objects and settings (printer/process/filament presets, per-object settings) replace the current project.

| param | type | required | description |
|---|---|---|---|
| `discard_unsaved` | boolean |  | Don't ask about unsaved changes in the current project. Default false. |
| `path` | string | yes | Absolute path of the .3mf file. |

### `save_project`

Save the project as a .3mf file (without dialogs).

| param | type | required | description |
|---|---|---|---|
| `path` | string |  | Absolute .3mf path. Default: the project's current file. |

### `load_model`  (H)

Import model files (STL, 3MF, OBJ, STEP, AMF, SVG) into the current project, like File > Import. With a 3MF its settings are loaded too unless load_settings is false. Returns the new objects.

| param | type | required | description |
|---|---|---|---|
| `append` | boolean |  | Keep the existing objects (default true). false = start a new project first. |
| `load_settings` | boolean |  | For 3MF: also load its presets/settings (default true). |
| `path` | string |  | Absolute path of a model file. |
| `paths` | array of string |  | Several files at once. |

### `undo`

Undo the last change (like Ctrl+Z).

No parameters.

### `redo`

Redo the last undone change (like Ctrl+Y).

No parameters.

## Objects

### `list_objects`  (RO, H)

All objects with id, name, plate, position/rotation/scale of the first instance, size, filament slot, per-object settings, and whether they are outside the printable volume.

No parameters.

### `remove_object`  (H)

Delete objects by id (like selecting them and pressing Delete).

| param | type | required | description |
|---|---|---|---|
| `id` | integer |  | Object id (see list_objects). |
| `ids` | array of integer |  | Several object ids. |

### `clear_project`  (H)

Delete all objects (plates and settings stay).

No parameters.

### `select_objects`

Select objects in the 3D view and object list (UI selection, like clicking them). Empty = clear.

| param | type | required | description |
|---|---|---|---|
| `all` | boolean |  | Select all objects. |
| `ids` | array of integer |  | Object ids. |

### `transform_object`

Transform an object like the move/rotate/scale fields in the sidebar (undoable; the plate is re-assigned and the object stays on the bed). position = absolute [x,y] (or [x,y,z]) of the bounding-box center in mm, bed coordinates; move_by = relative offset in mm; rotate_deg = relative rotation [rx,ry,rz] in degrees around the world axes; scale = absolute factor (number = uniform, or [sx,sy,sz], along the object's own axes); size_mm = target [x,y,z] size along the object's own axes. Applied in the order rotate, scale/size, move.

| param | type | required | description |
|---|---|---|---|
| `id` | integer | yes | Object id (see list_objects). |
| `move_by` | array of number |  | [dx, dy] or [dx, dy, dz] in mm. |
| `position` | array of number |  | [x, y] or [x, y, z] in mm. |
| `rotate_deg` | array of number |  | [rx, ry, rz] in degrees. |
| `scale` | any |  | Number or [sx, sy, sz]; 1 = original size. |
| `size_mm` | array of number |  | [x, y, z] in mm. |

### `set_object_settings`

Override process settings for one object (like adding settings to an object in the object list), e.g. {"sparse_infill_density": "40%", "wall_loops": "4"}. A null value removes that override.

| param | type | required | description |
|---|---|---|---|
| `id` | integer | yes | Object id (see list_objects). |
| `values` | object | yes | Option key -> value; null removes. |

### `set_object_filament`

Print an object with filament slot N (1-based), like the object list's filament column.

| param | type | required | description |
|---|---|---|---|
| `filament` | integer | yes | Filament slot (1-based). |
| `id` | integer | yes | Object id (see list_objects). |

### `rename_object`

Rename an object.

| param | type | required | description |
|---|---|---|---|
| `id` | integer | yes | Object id (see list_objects). |
| `name` | string | yes | New name. |

### `clone_object`

Make copies of an object (like Clone in the object's context menu). Each copy is a new object placed next to the original.

| param | type | required | description |
|---|---|---|---|
| `copies` | integer | yes | Number of copies to add (1..100). |
| `id` | integer | yes | Object id (see list_objects). |

### `arrange_objects`  (H)

Auto-arrange the objects (like the Arrange toolbar button) and wait for it. Like in the app, objects that do not fit spill onto further plates and empty plates are removed.

No parameters.

### `orient_objects`

Auto-orient objects for printing (the given ids, or all), and wait.

| param | type | required | description |
|---|---|---|---|
| `ids` | array of integer |  | Object ids (default: all). |

## Plates

### `list_plates`  (RO)

All plates: number, name, bed type, print sequence, objects on it, and slice summary.

No parameters.

### `add_plate`

Add an empty plate (like the + plate button). The new plate becomes current.

No parameters.

### `delete_plate`

Delete a plate and the objects on it.

| param | type | required | description |
|---|---|---|---|
| `plate` | integer | yes | Plate number (1-based, as labelled in the app). Default: the current plate. |

### `select_plate`

Make a plate the current one (like clicking it).

| param | type | required | description |
|---|---|---|---|
| `plate` | integer | yes | Plate number (1-based, as labelled in the app). Default: the current plate. |

### `set_plate_settings`

Set a plate's name, bed type ('default' = use the global bed type) or print sequence.

| param | type | required | description |
|---|---|---|---|
| `bed_type` | string |  | Bed type name (see get_selected_presets.available_bed_types) or 'default'. |
| `name` | string |  | Plate name. |
| `plate` | integer |  | Plate number (1-based, as labelled in the app). Default: the current plate. |
| `print_sequence` | `default` \| `by layer` \| `by object` |  | Print sequence. |

### `move_object_to_plate`

Move an object onto another plate (centered on it; use arrange_objects afterwards to tidy).

| param | type | required | description |
|---|---|---|---|
| `id` | integer | yes | Object id (see list_objects). |
| `plate` | integer | yes | Plate number (1-based, as labelled in the app). Default: the current plate. |

## Slicing and export

### `slice`  (H)

Slice the current plate (or 'plate', or all plates) like the Slice button, switch to the Preview and wait for the result (print time, filament, warnings). Sends progress notifications.

| param | type | required | description |
|---|---|---|---|
| `all_plates` | boolean |  | Slice all plates. Default false. |
| `plate` | integer |  | Plate number (1-based, as labelled in the app). Default: the current plate. |
| `timeout_s` | number |  | Max seconds to wait (default 900). |
| `wait` | boolean |  | Wait for the result (default true). |

### `get_slice_status`  (RO, H)

Whether slicing is running, and the slice result of each plate.

No parameters.

### `cancel_slice`  (H)

Stop the running slice (like the Cancel button on the slicing notification).

No parameters.

### `export_gcode`  (H)

Write the sliced G-code of the current plate to 'path' (slice first). Waits until the file is written.

| param | type | required | description |
|---|---|---|---|
| `path` | string | yes | Absolute output path (.gcode). |
| `timeout_s` | number |  | Max seconds to wait (default 300). |

### `export_sliced_file`

Write a .gcode.3mf (G-code plus thumbnails and slice info, the file the printer takes) of the current plate or all plates.

| param | type | required | description |
|---|---|---|---|
| `all_plates` | boolean |  | All sliced plates. Default false. |
| `path` | string | yes | Absolute output path (.gcode.3mf). |

## Printers

### `list_printers`  (RO)

Printers discovered on the LAN (SSDP) plus the selected one: id (serial), name, IP, model, online/connected, busy.

No parameters.

### `get_printer_status`  (RO)

Live status of a printer (the selected one by default): print state and progress, temperatures, AMS trays, HMS errors, and the latest print_plate operation sent to it (last_print_operation, see get_print_operation). Read-only.

| param | type | required | description |
|---|---|---|---|
| `printer` | string |  | Printer serial, name or IP from list_printers. Default: the selected printer. |

### `connect_printer`

Select a discovered LAN printer and connect to it (like choosing it on the Device tab). Pass the printer's access code the first time; it is stored by the app like a manual entry. Read-only otherwise: nothing is started or moved.

| param | type | required | description |
|---|---|---|---|
| `access_code` | string |  | 8-character LAN access code from the printer's screen (only needed once). |
| `printer` | string | yes | Printer serial, name or IP from list_printers. Default: the selected printer. |
| `wait_s` | number |  | Seconds to wait for the connection (default 20). |

### `disconnect_printer`

Disconnect from the selected printer (LAN MQTT). Nothing on the printer changes.

No parameters.

### `printer_camera_snapshot`  (RO)

Save one frame from a printer's camera as a PNG (needs ffmpeg on the computer; LAN RTSPS stream).

| param | type | required | description |
|---|---|---|---|
| `path` | string | yes | Absolute output path (.png). |
| `printer` | string |  | Printer serial, name or IP from list_printers. Default: the selected printer. |
| `timeout_s` | number |  | Give up after this many seconds (default 20, 3..60). |

## Print jobs (gated by printer control and human approval)

### `print_plate`

Send one sliced plate for one explicit human print request, without an approval popup. Default dry_run=true returns readiness, job summary and job_fingerprint. A real send requires printer control, dry_run=false, fingerprint, user_authorized=true and a stable authorization_id identifying that human request. Reuse the same ID for retries; it is persisted and consumed before the first send attempt, including failures. Never generate another ID unless the human issues another print instruction. Changed job facts are rejected. Poll get_print_operation for the result. An explicit human instruction such as 'Print this' or 'Go ahead and now print' authorizes exactly one print of the identified job. Do not ask again in chat or require an approval popup. Multiple confirmations of the same pending job do not authorize multiple prints. Before sending, complete Sync Info and filament synchronization, verify printer readiness and native feeder labels such as HT-A, prepare and slice the requested plate, then run print_plate dry_run=true and inspect the actual Print dialog mapping. Report the job, printer, material, mapping, time and usage as information, not another approval question. Call print_plate with dry_run=false, fingerprint, user_authorized=true and authorization_id. Use a stable unique ID for the human request (conversation/message ID or a recorded UUID), containing only ASCII letters, digits, hyphens or underscores. Reuse it across retries, reconnections and restarts; never derive it from job facts or mint another for the same request. The server persists consumption before sending and rejects reuse with approval.used. A failed or uncertain send must never trigger another print automatically. New prints need a new explicit human instruction. Printer control must be enabled with human authorization. Changes outside the requested scope or ambiguous jobs still require clarification. Fingerprints and final transport checks reject changed job, printer, mapping or settings. Print/Send controls remain unavailable to generic dialog tools; use print_plate. Poll get_print_operation and report started only when the printer reports the matching job. The server trusts the agent to attest the human request; an ID is replay protection, not independent authentication of chat intent. The headless server has no print tools and never sends.

| param | type | required | description |
|---|---|---|---|
| `authorization_id` | string |  | Stable unique ID for that human request, 1..128 ASCII letters, digits, hyphens or underscores. Reuse across retries and restarts; never derive from the job fingerprint. |
| `dry_run` | boolean |  | Only check readiness and return the summary and job_fingerprint (default true). |
| `fingerprint` | string |  | job_fingerprint from the current dry run. Required with dry_run=false. |
| `open_dialog` | boolean |  | Open the Print dialog without sending (default false). |
| `user_authorized` | boolean |  | True only for an explicit human request to print this job; required for a real send. |


### `get_print_operation`  (RO)

Outcome of a print_plate send, by operation_id (default: the latest). status is one of: send_requested, awaiting_user_confirmation (the Print dialog asks the user something), uploading, transferred_awaiting_printer, printing, paused, finished, printer_failed, failed (transfer failed or was cancelled), not_started (no transfer began; read the Print dialog's message), test_no_send, unknown. printing/paused/finished require the printer to report a matching new job. Unknown means unknown: inspect the printer and ask the user; never resend automatically. Read-only.

| param | type | required | description |
|---|---|---|---|
| `operation_id` | string |  | operation_id from print_plate (default: the latest). |

### `printer_control`

Pause, resume or stop the selected printer's current print. Default dry_run = true (reports what would be sent). A real command needs dry_run = false, confirm = true and printer control enabled by the user.

| param | type | required | description |
|---|---|---|---|
| `action` | `pause` \| `resume` \| `stop` | yes | Command. |
| `confirm` | boolean |  | Required with dry_run = false. |
| `dry_run` | boolean |  | Default true. |
| `printer` | string |  | Printer serial, name or IP from list_printers. Default: the selected printer. |

## UI

### `ui_get_state`  (RO)

Current tab, 3D/preview view, selected objects and open dialogs.

No parameters.

### `ui_switch_tab`

Show a main tab: home, prepare (3D editor), preview or device (printer monitor).

| param | type | required | description |
|---|---|---|---|
| `tab` | `home` \| `prepare` \| `preview` \| `device` | yes | Tab. |

### `ui_set_camera`

Point the 3D/preview camera: a standard view and/or zoom to the bed or the objects.

| param | type | required | description |
|---|---|---|---|
| `view` | `iso` \| `top` \| `bottom` \| `front` \| `rear` \| `left` \| `right` |  | Standard view. |
| `zoom` | `bed` \| `objects` |  | Zoom to fit. |

### `ui_screenshot`  (RO)

Save a PNG of the 3D/preview canvas (default), the whole window, or a dialog, so you can check what the app shows.

| param | type | required | description |
|---|---|---|---|
| `dialog` | any |  | For target=dialog: index or title from ui_list_dialogs (default: the topmost). |
| `path` | string | yes | Absolute output path (.png). |
| `target` | `canvas` \| `window` \| `dialog` |  | What to capture (default canvas). |

### `ui_list_dialogs`  (RO)

Open dialogs and windows (index, title, class, modal).

No parameters.

### `ui_dialog_inspect`  (RO)

List a dialog's text, fields, checkboxes, choices and buttons, numbered as 'widget'. For the Print dialog, also return the actual destination printer and filament mapping for the approval summary.

| param | type | required | description |
|---|---|---|---|
| `dialog` | any |  | Dialog index or (part of the) title from ui_list_dialogs. Default: the topmost. |

### `ui_dialog_click`

Click a button in a dialog by label (e.g. 'OK', 'Don't save', 'Transfer') or widget number. Refused while a Print / Send dialog is open (prints go through print_plate with a single-use human chat authorization).

| param | type | required | description |
|---|---|---|---|
| `button` | string |  | Button label (case-insensitive). |
| `dialog` | any |  | Dialog index or (part of the) title from ui_list_dialogs. Default: the topmost. |
| `widget` | integer |  | Widget number. |


### `ui_dialog_set`

Set a text field, checkbox, radio button or choice in a dialog (widget number from ui_dialog_inspect). Refused while a Print / Send dialog is open.

| param | type | required | description |
|---|---|---|---|
| `dialog` | any |  | Dialog index or (part of the) title from ui_list_dialogs. Default: the topmost. |
| `value` | any | yes | Text, boolean, or choice string. |
| `widget` | integer | yes | Widget number. |

### `ui_close_dialog`

Close a dialog like its window close button (Cancel).

| param | type | required | description |
|---|---|---|---|
| `dialog` | any |  | Dialog index or (part of the) title from ui_list_dialogs. Default: the topmost. |

### `ui_open_dialog`

Open an app dialog for the user or for inspection: preferences, printers (select printers), filaments (select filaments), printer_settings, process_settings, filament_settings, shortcuts, print (Print dialog, sends nothing), send (Send to printer storage, sends nothing), sync_info (the sidebar Sync Info action for nozzle/AMS information), sync_filaments (the Continue to sync filaments action). Complete filament synchronization before slicing and presenting each print approval. Inspect and handle any dialogs; opening the workflow does not prove synchronization completed. Sync can change presets and invalidate slices.

| param | type | required | description |
|---|---|---|---|
| `dialog` | `preferences` \| `printers` \| `filaments` \| `printer_settings` \| `process_settings` \| `filament_settings` \| `shortcuts` \| `print` \| `send` \| `sync_info` \| `sync_filaments` | yes | Dialog. |

## Headless differences

The headless server's versions of the shared tools take slightly different parameters:

- `select_presets`: headless only: `allow_incompatible` (Allow presets marked incompatible with the printer. Default false.).
- `load_model`: headless only: `arrange` (Place objects on the bed. Default: true for meshes, false for 3MF.). live only: `load_settings`, `paths`.
- `remove_object`: live only: `ids`.
- `arrange_objects`: headless only: `spacing_mm` (Minimum gap between objects in mm (default 6).).
- `slice`: live only: `all_plates`, `plate`.
- `export_gcode`: headless only: `overwrite` (Replace an existing file (default true).), `slice_if_needed` (Slice first when needed (default true).).
