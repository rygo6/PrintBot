#!/usr/bin/env bash
# Launch the print_bot (LAN-only Bambu Studio fork) from the local build tree.
# Settings live in ${XDG_CONFIG_HOME:-~/.config}/PrintBot, separate from any
# stock Bambu Studio install. The open network plugin is loaded from
# build/src/plugins next to the executable.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe="$here/build/src/bambu-studio"
if [[ ! -x "$exe" ]]; then
    echo "print_bot: $exe not found; build first (see PRINT_BOT.md)" >&2
    exit 1
fi
# Keep stray Python/AppImage environments out of the process.
unset PYTHONHOME PYTHONPATH APPDIR APPIMAGE LD_LIBRARY_PATH
# Bambu Studio's GL canvas is X11-only; run through XWayland.
export GDK_BACKEND="${GDK_BACKEND:-x11}"
# The statically linked curl has no default CA bundle; point it at the system one.
if [[ -z "${SSL_CERT_FILE:-}" && -f /etc/ssl/certs/ca-certificates.crt ]]; then
    export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
fi
exec "$exe" "$@"
