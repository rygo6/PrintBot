"""Shared paths/settings for the Linux fuzz scripts. Everything defaults to repo-relative locations and can be
overridden with environment variables (or the matching command-line flags of fuzz.py / repro.py).

  PRINTBOT_FUZZ_APP      sanitizer GUI binary          (default: <repo>/build-asan/src/bambu-studio)
  PRINTBOT_FUZZ_MCP      print_bot-mcp relay binary    (default: <repo>/build/src/print_bot-mcp, falls back to the ASan build)
  PRINTBOT_FUZZ_WORK     output/scratch directory      (default: <repo>/temp/fuzz; runs, crashes, sandboxes, seed home)
  PRINTBOT_FUZZ_CORPUS   input corpus                  (default: $WORK/corpus, created by gen_corpus.py)
  PRINTBOT_FUZZ_DISPLAY  Xvfb display                  (default: :77)
  PRINTBOT_FUZZ_SYSROOT  optional user-space sysroot with xdotool and its libs (<sysroot>/usr/bin, <sysroot>/usr/lib/<triple>)
  ASAN_SYMBOLIZER_PATH   llvm-symbolizer               (default: first llvm-symbolizer found)
"""
import os, glob, shutil, subprocess

FZ = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(FZ, '..'))


def _env(name, default):
    v = os.environ.get(name)
    return os.path.abspath(v) if v else default


APP = _env('PRINTBOT_FUZZ_APP', os.path.join(REPO, 'build-asan', 'src', 'bambu-studio'))
_mcp_default = os.path.join(REPO, 'build', 'src', 'print_bot-mcp')
if not os.path.exists(_mcp_default):
    _mcp_default = os.path.join(os.path.dirname(APP), 'print_bot-mcp')
MCPBIN = _env('PRINTBOT_FUZZ_MCP', _mcp_default)
WORK = _env('PRINTBOT_FUZZ_WORK', os.path.join(REPO, 'temp', 'fuzz'))
CORPUS = _env('PRINTBOT_FUZZ_CORPUS', os.path.join(WORK, 'corpus'))
DISP = os.environ.get('PRINTBOT_FUZZ_DISPLAY', ':77')
SYSROOT = os.environ.get('PRINTBOT_FUZZ_SYSROOT', '')
SUPP = os.path.join(FZ, 'ubsan.supp')
SSL_CERT_FILE = next((p for p in ['/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt',
                                  '/etc/ssl/cert.pem'] if os.path.exists(p)), '')

# Printer / print-job MCP tools are never called by the fuzzers: they could reach a real printer on the network
# (start, stop or change a job, move axes, heat). The app also runs in a network namespace with no network
# (unshare -cn), but the tool filter is the first line of defence and also keeps the fuzz on the slicer/GUI.
BLOCKED_TOOLS = {'list_printers', 'get_printer_status', 'connect_printer', 'disconnect_printer',
                 'printer_camera_snapshot', 'print_plate', 'get_print_operation', 'printer_control',
                 'authorize_print', 'send_to_printer', 'upload_to_printer'}
# Belt and braces: any tool whose name looks printer/job related is filtered too, so new tools are excluded by default.
import re as _re
BLOCK_RE = _re.compile(r'printer|print_plate|print_job|send_|upload|camera|authoriz', _re.I)


def tool_allowed(name):
    return name not in BLOCKED_TOOLS and not BLOCK_RE.search(name)


def symbolizer():
    p = os.environ.get('ASAN_SYMBOLIZER_PATH') or shutil.which('llvm-symbolizer')
    if not p:
        c = sorted(glob.glob('/usr/lib/llvm-*/bin/llvm-symbolizer'))
        p = c[-1] if c else ''
    return p


def xdotool():
    if SYSROOT and os.path.exists(os.path.join(SYSROOT, 'usr/bin/xdotool')):
        return os.path.join(SYSROOT, 'usr/bin/xdotool')
    return shutil.which('xdotool') or 'xdotool'


def x_env(extra=None):
    """Environment for X helper tools (xdotool/xprop) on the fuzz display."""
    env = dict(os.environ, DISPLAY=DISP)
    if SYSROOT:
        libs = glob.glob(os.path.join(SYSROOT, 'usr/lib/*-linux-gnu'))
        if libs: env['LD_LIBRARY_PATH'] = ':'.join(libs)
    if extra: env.update(extra)
    return env


def stub_bin_dir():
    """Directory of fake browser / file-manager launchers so the app never opens a real browser or URL."""
    d = os.path.join(WORK, 'bin'); os.makedirs(d, exist_ok=True)
    body = '#!/bin/sh\necho "$(date +%T) open $*" >> "${FUZZ_SB:-/tmp}/xdg-open.log"\nexit 0\n'
    for n in ['xdg-open', 'gio', 'gvfs-open', 'x-www-browser', 'sensible-browser', 'firefox', 'google-chrome',
              'chromium', 'nautilus']:
        p = os.path.join(d, n)
        if not os.path.exists(p):
            open(p, 'w').write(body); os.chmod(p, 0o755)
    return d


def lsan_shim():
    """Build (once) the LD_PRELOAD shim that runs a recoverable LeakSanitizer check on request."""
    so = os.path.join(WORK, 'lsan_trigger.so'); src = os.path.join(FZ, 'lsan_trigger.c')
    if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
        os.makedirs(WORK, exist_ok=True)
        cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
        subprocess.run([cc, '-shared', '-fPIC', '-O1', '-o', so, src, '-ldl', '-lpthread'], check=True)
    return so


def wait_display_free(timeout=30):
    import time
    lock = '/tmp/.X%s-lock' % DISP.lstrip(':')
    for _ in range(int(timeout * 2)):
        if not os.path.exists(lock): return True
        time.sleep(0.5)
    return False
