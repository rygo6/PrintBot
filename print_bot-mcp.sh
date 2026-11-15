#!/usr/bin/env bash
# Launch the print_bot MCP server (stdio, JSON-RPC 2.0) from the local build tree.
# stdout carries only MCP protocol messages; logs go to stderr.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe="$here/build/src/print_bot-mcp"
if [[ ! -x "$exe" ]]; then
    echo "print_bot-mcp: $exe not found; build first (see PRINT_BOT.md)" >&2
    exit 1
fi
unset PYTHONHOME PYTHONPATH APPDIR APPIMAGE LD_LIBRARY_PATH
exec "$exe" --resources "$here/resources" "$@"
