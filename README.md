# PrintBot

**PrintBot is a fork of [Bambu Studio](https://github.com/bambulab/BambuStudio) with the bloat and all
of Bambu's closed, proprietary code removed.** Bambu Cloud, the account login and the closed Bambu
networking plugin are gone. PrintBot is LAN-only: it talks to printers on your local network through the
open-source [open-bamboo-networking](https://github.com/ClusterM/open-bamboo-networking) plugin, which is
vendored in `bambu_networking/` and built with the app. It also adds an [MCP](https://modelcontextprotocol.io)
server so AI agents can drive the slicer, with printing behind a human approval step.

PrintBot is based on Bambu Studio 02.08.04.57. Bambu Studio is based on
[PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from
[Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. PrintBot is
not affiliated with or supported by Bambu Lab; please don't report PrintBot problems to Bambu.

Detailed fork notes, the build and run instructions and the full MCP reference are in
[PRINT_BOT.md](PRINT_BOT.md).

## What still works

- The Prepare and Preview tabs: slicing, the G-code viewer, multiple plates, auto-arrange and auto-orient,
  supports (normal/tree/hybrid), multi-material painting, global/object/part settings, arc fitting,
  3MF/STL/STEP/OBJ import and export.
- All Bambu Lab printer, process and filament presets, including multi-nozzle and multi-extruder printers
  (H2D, H2D Pro, H2C, H2S, X2D). Nothing multi-extruder was removed.
- LAN printers: discovery, adding a printer by IP and access code, upload, start, pause/resume/stop.
- The Device tab: live status, controls, native AMS view, camera, HMS error texts and the printer's
  self-calibration.
- Preferences, translations and dark mode.

The printer must be in **LAN Only** mode with **Developer Mode** enabled (printer screen: Settings →
Network/General). Settings are stored in `~/.config/PrintBot`, separate from any stock Bambu Studio install.

## What's been removed

Cloud features are stripped by the CMake option `PRINTBOT_LAN_ONLY` (default `ON`) in three layers: the
app's `NetworkAgent` (no cloud call reaches the plugin), the GUI, and the plugin itself (cloud forced off,
every cloud endpoint pointed at `cloud-disabled.invalid`). Most of the code behind them has also been
deleted from the tree:

- **Bambu Cloud and account login:** login/logout, registration, avatars, printer binding and ping-code
  binding, region reports, cloud printing and print history, cloud multi-machine printing and the task
  manager, cloud preset sync and user setting upload, Go Live/virtual camera streaming.
- **MakerWorld and the store:** home-page models, model mall, publishing, ratings and model import, MakerLab,
  Maker's Supply, staff picks and the academy list.
- **The closed Bambu networking plugin:** its automatic download and install, plugin update checks and the
  network test dialog. The open-bamboo-networking plugin replaces it and is loaded first from the build.
- **Telemetry:** tracking, consent reporting, the privacy-version check and the User Experience Improvement
  Program.
- **Helio** and the **Filament Manager** (cloud spool catalogue).
- **WebKit and every webview:** no wxWebView, WebKitGTK, libsoup or JavaScriptCore is linked, and
  `resources/web/` is gone. The web Home page is replaced by a native page (New/Open Project and recent
  projects). The web filament create/edit dialogs, purging-volumes dialog, nozzle table, Device page and AMS
  panel were all replaced by native wxWidgets versions.
- **The setup wizard:** the webview wizard and its terms dialog. A native "Printers and filaments" dialog
  replaces it, and a first run selects the P2S with default Bambu filaments without a wizard.
- **The Project tab** (web model/project info page).
- **The Calibration tab** and its wizards (temperature, flow rate, pressure advance, retraction, max flow
  rate, VFA). The Device tab's printer self-calibration stays.
- **The Home page User Manual:** the in-app wiki, tutorial cards and wiki search.
- **Update checks:** Studio version checks, the "install new Bambu Studio" prompt and the app updater,
  online preset, resource and printer-config downloads, and HMS text downloads (bundled texts are used).
  Bundled profiles and local config updates still work.
- **Other printers and hosts:** non-Bambu vendor profiles, OctoPrint/Duet/Repetier/MKS/FlashAir/AstroBox
  print hosts, Bonjour and the physical-printer dialog.
- **Other unused code:** the SLA support and hollowing tools in the GUI, the OpenCSG and WebView2
  dependencies, never-linked legacy code, unreferenced images, Docker/release clutter and test sandboxes.

[PRINT_BOT.md](PRINT_BOT.md) lists the deleted files in detail.

## MCP server

PrintBot includes an MCP server (`src/mcp/`, `src/core/`) so an AI agent can operate the slicer.

- **How agents connect:** the agent launches `print_bot-mcp` (or the `print_bot-mcp.sh` wrapper), a stdio
  server speaking newline-delimited JSON-RPC 2.0. Register it in your MCP client like this:

  ```json
  { "mcpServers": { "print_bot": { "command": "/path/to/print_bot/print_bot-mcp.sh" } } }
  ```

- **Live GUI mode (default):** the running PrintBot app hosts an endpoint on a private Unix socket
  (owner-only permissions, peer user checked), and `print_bot-mcp` acts as a bridge to it. Tools run on the
  app's main thread through the same code paths as your own clicks, so the window shows everything the agent
  does, with an "Agent: ..." notice on the canvas.
- **Headless mode:** with no app running (or `--headless`), `print_bot-mcp` runs the GUI-free core itself.
  Headless mode can work with presets, load models, slice and export G-code, and it never talks to a printer.
- **Tool groups:** status and events, presets (list, select, edit, save), project (new, open, save, load
  model, undo/redo), objects (transform, per-object settings and filament, clone, arrange, orient), plates,
  slicing and export, printers (LAN discovery, status, connect, camera snapshot), print jobs, and UI control
  (tabs, camera view, screenshots, dialogs). There are 58 tools in live mode and 17 in headless mode.
- **Safety model:**
  - Printer control is **off by default**. Starting, pausing, resuming or stopping a print is refused unless
    you start the app with `PRINTBOT_MCP_ALLOW_PRINTER_CONTROL=1` or set `mcp_allow_printer_control = true`.
  - Every print goes through one gate. The agent first runs a dry run that returns the job summary and a
    fingerprint of the exact job. A real send needs that fingerprint, and then PrintBot shows its own
    **Approve print** dialog with the job, printer, filament mapping and estimates.
  - Only a human click approves a print. The agent can't see or click that dialog, or any control in the
    Print/Send dialogs.
  - If anything in the job changes after approval, nothing is sent. Each approval is single-use and expires.
  - A send is reported as a pending operation that the agent tracks, never as a confirmed print.
- **Agent skill:** [`skills/printbot/`](skills/printbot/) has a skill that teaches an agent the workflows,
  safety rules and error codes, plus a generated parameter reference for every tool.

## Building

Linux (GTK3) is the supported platform. See [PRINT_BOT.md](PRINT_BOT.md) for the dependency and app build
commands; the open networking plugin is built with the app. Run with `./print_bot.sh`.

## License

PrintBot is licensed under the GNU Affero General Public License, version 3, like the projects it is
based on. Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is
based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by
Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro
Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any
way (even behind a web server), your software must be released under the same license.

The open-bamboo-networking plugin in `bambu_networking/` is licensed under the GNU Affero General Public
License, version 3 or later (see `bambu_networking/LICENSE`). PrintBot does not use or ship Bambu's closed
networking plugin, which upstream Bambu Studio describes as based on non-free libraries.
