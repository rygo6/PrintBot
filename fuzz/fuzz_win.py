#!/usr/bin/env python3
"""PrintBot chaos GUI + MCP fuzzer for Windows (MSVC ASan build).
Input goes only to PrintBot's own windows via PostMessage (never SendInput, never other apps).
Isolated data dir (--datadir) per run, no printer configured, printer/job MCP tools filtered."""
import os, sys, time, json, random, subprocess, threading, re, glob, shutil, argparse, ctypes
from ctypes import wintypes as W

FZ = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(FZ, '..'))
ap = argparse.ArgumentParser(description='Chaos GUI + MCP fuzzer for the Windows MSVC /fsanitize=address build.')
ap.add_argument('--minutes', type=float, default=40)
ap.add_argument('--tag', default='win')
ap.add_argument('--app', default=os.path.join(REPO, 'build-asan', 'src', 'RelWithDebInfo', 'bambu-studio.exe'))
ap.add_argument('--mcp', default=os.path.join(REPO, 'build', 'src', 'Release', 'print_bot-mcp.exe'))
ap.add_argument('--fresh-prob', type=float, default=0.15)
ap.add_argument('--work', default=os.environ.get('PRINTBOT_FUZZ_WORK') or os.path.join(REPO, 'temp', 'fuzz'),
                help='output directory (env PRINTBOT_FUZZ_WORK, default <repo>\\temp\\fuzz)')
ap.add_argument('--corpus', default=os.environ.get('PRINTBOT_FUZZ_CORPUS'), help='input corpus (default <work>\\corpus; create with gen_corpus.py)')
ap.add_argument('--modal-mcp-prob', type=float, default=0.15, help='chance to fire a state-changing MCP call while a modal is open')
args = ap.parse_args()
APP, MCPBIN = os.path.abspath(args.app), os.path.abspath(args.mcp)
WORK = os.path.abspath(args.work); os.makedirs(WORK, exist_ok=True)
CORPUS = os.path.abspath(args.corpus or os.path.join(WORK, 'corpus'))
for _p, _what in [(APP, '--app'), (MCPBIN, '--mcp')]:
    if not os.path.exists(_p): sys.exit(f'{_what}: {_p} does not exist (build it or pass the path)')
if not os.path.isdir(CORPUS) or not os.listdir(CORPUS):
    sys.exit(f'corpus {CORPUS} is empty: run gen_corpus.py first')
# Printer / print-job tools are never called: they could reach a real printer on the network (see README.md).
BLOCKED_TOOLS = {'list_printers', 'get_printer_status', 'connect_printer', 'disconnect_printer',
                 'printer_camera_snapshot', 'print_plate', 'get_print_operation', 'printer_control',
                 'authorize_print', 'send_to_printer', 'upload_to_printer'}
BLOCK_RE = re.compile(r'printer|print_plate|print_job|send_|upload|camera|authoriz', re.I)
READONLY_RE = re.compile(r'^(get_|list_|ui_list|ui_screenshot|ui_dialog_inspect|ui_inspect|describe|version)', re.I)
OUT = os.path.join(WORK, 'runs_' + args.tag); os.makedirs(OUT, exist_ok=True)
CRASHES = os.path.join(WORK, 'crashes_' + args.tag); os.makedirs(CRASHES, exist_ok=True)
SEED = os.path.join(WORK, 'seed_' + args.tag)
PROGRESS = os.path.join(WORK, 'progress_' + args.tag + '.txt')
stats = {'runs': 0, 'ui_actions': 0, 'mcp_calls': 0, 'mcp_errors': 0, 'mcp_busy': 0, 'crashes': 0, 'hangs': 0,
         'clean_exits': 0, 'start': time.time()}

def log(*a):
    s = time.strftime('%H:%M:%S') + ' ' + ' '.join(str(x) for x in a)
    print(s, flush=True)
    with open(PROGRESS, 'a', encoding='utf-8') as f: f.write(s + '\n')

# ---------------- Win32 ----------------
u32 = ctypes.WinDLL('user32', use_last_error=True); g32 = ctypes.WinDLL('gdi32'); k32 = ctypes.WinDLL('kernel32')
WNDENUMPROC = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
u32.EnumWindows.argtypes = [WNDENUMPROC, W.LPARAM]
u32.EnumChildWindows.argtypes = [W.HWND, WNDENUMPROC, W.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]; u32.GetWindowThreadProcessId.restype = W.DWORD
u32.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u32.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, ctypes.POINTER(ctypes.c_size_t)]
u32.ChildWindowFromPointEx.argtypes = [W.HWND, W.POINT, W.UINT]; u32.ChildWindowFromPointEx.restype = W.HWND
u32.ScreenToClient.argtypes = [W.HWND, ctypes.POINTER(W.POINT)]
u32.ClientToScreen.argtypes = [W.HWND, ctypes.POINTER(W.POINT)]
u32.GetWindowRect.argtypes = [W.HWND, ctypes.POINTER(W.RECT)]
u32.GetClientRect.argtypes = [W.HWND, ctypes.POINTER(W.RECT)]
u32.IsHungAppWindow.argtypes = [W.HWND]
u32.SetWindowPos.argtypes = [W.HWND, W.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.UINT]
u32.GetWindow.argtypes = [W.HWND, W.UINT]; u32.GetWindow.restype = W.HWND
u32.GetParent.argtypes = [W.HWND]; u32.GetParent.restype = W.HWND
u32.GetDC.argtypes = [W.HWND]
u32.PrintWindow.argtypes = [W.HWND, W.HDC, W.UINT]
u32.SetProcessDPIAware()
u32.GetDC.restype = W.HDC; u32.ReleaseDC.argtypes = [W.HWND, W.HDC]
g32.CreateCompatibleDC.restype = W.HDC; g32.CreateCompatibleDC.argtypes = [W.HDC]
g32.CreateCompatibleBitmap.restype = W.HBITMAP; g32.CreateCompatibleBitmap.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int]
g32.SelectObject.restype = W.HGDIOBJ; g32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
g32.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, ctypes.c_void_p, ctypes.c_void_p, W.UINT]
g32.DeleteObject.argtypes = [W.HGDIOBJ]; g32.DeleteDC.argtypes = [W.HDC]
class GUITHREADINFO(ctypes.Structure):
    _fields_ = [('cbSize', W.DWORD), ('flags', W.DWORD), ('hwndActive', W.HWND), ('hwndFocus', W.HWND),
                ('hwndCapture', W.HWND), ('hwndMenuOwner', W.HWND), ('hwndMoveSize', W.HWND), ('hwndCaret', W.HWND),
                ('rcCaret', W.RECT)]
WM_SETTEXT, WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_COMMAND, WM_CLOSE = 0x000C, 0x0100, 0x0101, 0x0102, 0x0111, 0x0010
WM_MOUSEMOVE, WM_LBD, WM_LBU, WM_LBDBL, WM_RBD, WM_RBU, WM_MBD, WM_MBU, WM_WHEEL = 0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x207, 0x208, 0x20A
BM_CLICK = 0x00F5
MK_L, MK_R, MK_SHIFT, MK_CTRL, MK_M = 1, 2, 4, 8, 0x10
VK = {'Escape': 0x1B, 'Return': 0x0D, 'Tab': 0x09, 'Delete': 0x2E, 'BackSpace': 0x08, 'space': 0x20, 'Up': 0x26,
      'Down': 0x28, 'Left': 0x25, 'Right': 0x27, 'Page_Up': 0x21, 'Page_Down': 0x22, 'Home': 0x24, 'End': 0x23,
      'F2': 0x71, 'F5': 0x74, 'F10': 0x79, 'F11': 0x7A, 'plus': 0xBB, 'minus': 0xBD}
for ch in '0123456789ABCEFHLMOPRSTUWXZ': VK[ch.lower() if ch.isalpha() else ch] = ord(ch)

def cls(h):
    b = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, b, 256); return b.value
def title(h):
    b = ctypes.create_unicode_buffer(512); u32.GetWindowTextW(h, b, 512); return b.value
def pid_of(h):
    p = W.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p)); return p.value
def tid_of(h):
    return u32.GetWindowThreadProcessId(h, None)
def rect(h):
    r = W.RECT(); u32.GetWindowRect(h, ctypes.byref(r)); return r.left, r.top, r.right - r.left, r.bottom - r.top

def top_windows(pid):
    out = []
    def cb(h, _):
        if u32.IsWindowVisible(h) and pid_of(h) == pid: out.append(h)
        return True
    u32.EnumWindows(WNDENUMPROC(cb), 0)
    return out
def children(h):
    out = []
    def cb(c, _): out.append(c); return True
    u32.EnumChildWindows(h, WNDENUMPROC(cb), 0)
    return out

def ours(h, pid):
    return h and pid_of(h) == pid

def post(h, msg, wp, lp, pid):
    if ours(h, pid): u32.PostMessageW(h, msg, wp, lp)

def deepest(top, sx, sy):
    h = top
    for _ in range(30):
        pt = W.POINT(sx, sy); u32.ScreenToClient(h, ctypes.byref(pt))
        c = u32.ChildWindowFromPointEx(h, pt, 0x1 | 0x2)  # skip invisible|disabled
        if not c or c == h: return h
        h = c
    return h

def lp_xy(x, y): return ((y & 0xFFFF) << 16) | (x & 0xFFFF)

def mouse(pid, top, sx, sy, kind, btn=1, mods=0):
    h = deepest(top, sx, sy)
    pt = W.POINT(sx, sy); u32.ScreenToClient(h, ctypes.byref(pt)); lp = lp_xy(pt.x, pt.y)
    down, up, mk = {1: (WM_LBD, WM_LBU, MK_L), 2: (WM_RBD, WM_RBU, MK_R), 3: (WM_MBD, WM_MBU, MK_M)}[btn]
    post(h, WM_MOUSEMOVE, mods, lp, pid)
    if kind == 'click':
        post(h, down, mk | mods, lp, pid); post(h, up, mods, lp, pid)
    elif kind == 'dclick':
        post(h, WM_LBD, MK_L, lp, pid); post(h, WM_LBU, 0, lp, pid); post(h, WM_LBDBL, MK_L, lp, pid); post(h, WM_LBU, 0, lp, pid)
    return h

def drag(pid, top, a, b, btn=1, mods=0):
    h = deepest(top, *a)
    down, up, mk = {1: (WM_LBD, WM_LBU, MK_L), 2: (WM_RBD, WM_RBU, MK_R), 3: (WM_MBD, WM_MBU, MK_M)}[btn]
    def cl(x, y):
        pt = W.POINT(x, y); u32.ScreenToClient(h, ctypes.byref(pt)); return lp_xy(pt.x, pt.y)
    post(h, down, mk | mods, cl(*a), pid)
    for i in range(1, 7):
        post(h, WM_MOUSEMOVE, mk | mods, cl(a[0] + (b[0] - a[0]) * i // 6, a[1] + (b[1] - a[1]) * i // 6), pid); time.sleep(0.02)
    post(h, up, mods, cl(*b), pid)
    return h

def wheel(pid, top, sx, sy, n, mods=0):
    h = deepest(top, sx, sy)
    d = random.choice([120, -120])
    for _ in range(n): post(h, WM_WHEEL, ((d & 0xFFFF) << 16) | mods, lp_xy(sx, sy), pid)
    return h

def focus_of(top):
    gi = GUITHREADINFO(); gi.cbSize = ctypes.sizeof(GUITHREADINFO)
    if u32.GetGUIThreadInfo(tid_of(top), ctypes.byref(gi)) and gi.hwndFocus: return gi.hwndFocus
    return top

def key(pid, top, k):
    h = focus_of(top)
    vk = VK.get(k)
    if vk is None: return h
    post(h, WM_KEYDOWN, vk, 1, pid)
    if len(k) == 1: post(h, WM_CHAR, ord(k), 1, pid)
    elif k in ('Return', 'Tab', 'space', 'Escape', 'BackSpace'): post(h, WM_CHAR, {'Return': 13, 'Tab': 9, 'space': 32, 'Escape': 27, 'BackSpace': 8}[k], 1, pid)
    post(h, WM_KEYUP, vk, 0xC0000001, pid)
    return h

def type_text(pid, top, txt):
    h = focus_of(top)
    for c in txt: post(h, WM_CHAR, ord(c), 1, pid)
    return h

def screenshot(h, path):
    try:
        from PIL import Image
        x, y, w, hh = rect(h)
        if w <= 0 or hh <= 0: return
        hdc = u32.GetDC(None); mdc = g32.CreateCompatibleDC(hdc); bmp = g32.CreateCompatibleBitmap(hdc, w, hh)
        g32.SelectObject(mdc, bmp); u32.PrintWindow(h, mdc, 2)
        class BMI(ctypes.Structure):
            _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG), ('biPlanes', W.WORD),
                        ('biBitCount', W.WORD), ('biCompression', W.DWORD), ('biSizeImage', W.DWORD),
                        ('a', W.LONG), ('b', W.LONG), ('c', W.DWORD), ('d', W.DWORD)]
        bi = BMI(); bi.biSize = 40; bi.biWidth = w; bi.biHeight = -hh; bi.biPlanes = 1; bi.biBitCount = 32
        buf = ctypes.create_string_buffer(w * hh * 4)
        g32.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
        Image.frombuffer('RGB', (w, hh), buf, 'raw', 'BGRX', 0, 1).save(path)
        g32.DeleteObject(bmp); g32.DeleteDC(mdc); u32.ReleaseDC(None, hdc)
    except Exception as e:
        log('screenshot failed', e)

# ---------------- MCP ----------------
class Mcp:
    def __init__(self, sock, logf):
        self.p = subprocess.Popen([MCPBIN, '--gui', '--socket', sock, '--wait', '90'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=logf, text=True, encoding='utf-8', bufsize=1,
                                  creationflags=0x08000000)
        self.id = 0; self.lock = threading.Lock(); self.tools = []
    def call(self, method, params=None, timeout=120):
        with self.lock:
            self.id += 1
            msg = {'jsonrpc': '2.0', 'id': self.id, 'method': method}
            if params is not None: msg['params'] = params
            self.p.stdin.write(json.dumps(msg) + '\n'); self.p.stdin.flush()
            res = [None]
            def rd():
                while True:
                    line = self.p.stdout.readline()
                    if not line: return
                    try: j = json.loads(line)
                    except Exception: continue
                    if j.get('id') == self.id: res[0] = j; return
            t = threading.Thread(target=rd, daemon=True); t.start(); t.join(timeout)
            if t.is_alive(): raise TimeoutError(method)
            if res[0] is None: raise EOFError('mcp closed')
            return res[0]
    def notify(self, method):
        with self.lock:
            self.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': method}) + '\n'); self.p.stdin.flush()
    def close(self):
        try: self.p.kill()
        except Exception: pass

PATH_KEYS = re.compile(r'path|file|dir|output|dest', re.I)
def rand_string():
    return random.choice(['', 'a', 'Cube', 'x' * random.randint(1, 3000), '\u00e9\u4e2d\U0001F600', '..\\..', '%s%n%x',
                          '-1', 'nan', '1e309', ' ', '\n\t', 'true', 'null', '0', 'PLA', 'Bambu PLA Basic @BBL X1C',
                          'Bambu Lab X1 Carbon 0.4 nozzle', '0.20mm Standard @BBL X1C', 'Textured PEI Plate',
                          'by_layer', 'by_object', 'layer_height', 'sparse_infill_density', 'wall_loops',
                          'CON', 'C:\\Windows\\..\\nul', '\\\\?\\C:\\x', 'a:b'])
def rand_path(key):
    files = os.listdir(CORPUS)
    if random.random() < 0.6 and not re.search(r'out|export|save|dest', key, re.I):
        return os.path.join(CORPUS, random.choice(files))
    return os.path.join(SB_OUT, random.choice(['p.3mf', 'o.gcode', 'x.gcode.3mf', 'y.png', 'sub\\dir\\z.3mf', 'q', '', 'NUL', 'aux.3mf']))
def rand_value(schema, key='', depth=0):
    if not isinstance(schema, dict): schema = {}
    if random.random() < 0.06 or depth > 4:
        return random.choice([None, -1, 0, 1, 2**63, -2**31, 1e308, 3.5, '', 'zz', [], {}, True])
    if 'enum' in schema and random.random() < 0.85: return random.choice(schema['enum'])
    t = schema.get('type')
    if isinstance(t, list): t = random.choice(t)
    if t is None and 'anyOf' in schema: return rand_value(random.choice(schema['anyOf']), key, depth + 1)
    if t is None and 'oneOf' in schema: return rand_value(random.choice(schema['oneOf']), key, depth + 1)
    if t == 'string': return rand_path(key) if PATH_KEYS.search(key) else rand_string()
    if t == 'integer': return random.choice([0, 1, 1, 2, 3, -1, 5, 100, 2**31, random.randint(-5, 20)])
    if t == 'number': return random.choice([0, 0.1, 1, 2.5, -10, 45, 90, 180, 1000, 1e9, -1e-9, random.uniform(-200, 200)])
    if t == 'boolean': return random.random() < 0.5
    if t == 'array': return [rand_value(schema.get('items', {}), key, depth + 1) for _ in range(random.choice([0, 1, 1, 2, 3, 8]))]
    if t == 'object' or 'properties' in schema:
        props = schema.get('properties', {}); req = set(schema.get('required', [])); o = {}
        for k, s in props.items():
            if k in req or random.random() < 0.5: o[k] = rand_value(s, k, depth + 1)
        if random.random() < 0.05: o['bogus_' + str(random.randint(0, 9))] = 1
        if not props and random.random() < 0.5:
            o[random.choice(['layer_height', 'sparse_infill_density', 'wall_loops', 'enable_support', 'brim_type',
                             'nozzle_temperature', 'print_sequence', 'filament_diameter', 'xyz'])] = \
                random.choice(['0.1', '0', '-5', '100%', '1', 'nan', 'true', 'outer_only', 'by_object', '', 99999])
        return o
    return rand_string()
def mcp_args(tool):
    name = tool['name']; schema = tool.get('inputSchema', {})
    a = rand_value(schema, name) if random.random() < 0.9 else {}
    if not isinstance(a, dict): a = {}
    if name == 'load_model' and random.random() < 0.8:
        fs = os.listdir(CORPUS)
        for k, v in (schema.get('properties') or {}).items():
            if PATH_KEYS.search(k):
                a[k] = [os.path.join(CORPUS, random.choice(fs)) for _ in range(random.randint(1, 3))] \
                    if v.get('type') == 'array' else os.path.join(CORPUS, random.choice(fs))
    return a

# ---------------- run loop ----------------
class Run:
    def __init__(self, idx):
        self.idx = idx; self.dir = os.path.join(OUT, 'run%03d' % idx); os.makedirs(self.dir, exist_ok=True)
        self.actions = open(os.path.join(self.dir, 'actions.jsonl'), 'w', encoding='utf-8'); self.alog = []
        self.lock = threading.Lock(); self.t0 = time.time()
    def act(self, kind, **kw):
        rec = {'t': round(time.time() - self.t0, 2), 'k': kind, **kw}
        with self.lock:
            self.alog.append(rec); self.alog = self.alog[-400:]
            self.actions.write(json.dumps(rec, default=str) + '\n'); self.actions.flush()

def setup_sandbox(fresh):
    global SB, SB_OUT
    SB = os.path.join(WORK, 'sb_' + args.tag)
    if os.path.isdir(SB): shutil.rmtree(SB, ignore_errors=True)
    os.makedirs(SB, exist_ok=True)
    data = os.path.join(SB, 'data')
    if not fresh and os.path.isdir(SEED): shutil.copytree(SEED, data)
    for d in ['data', 'run', 'out', 'out\\sub', 'tmp', 'appdata', 'localappdata']: os.makedirs(os.path.join(SB, d), exist_ok=True)
    SB_OUT = os.path.join(SB, 'out')
    return data

def app_env(rundir):
    env = dict(os.environ)
    env.update({'APPDATA': os.path.join(SB, 'appdata'), 'LOCALAPPDATA': os.path.join(SB, 'localappdata'),
                'TEMP': os.path.join(SB, 'tmp'), 'TMP': os.path.join(SB, 'tmp'),
                'PRINTBOT_MCP_SOCKET': os.path.join(SB, 'run', 'mcp.sock'),
                'ASAN_OPTIONS': f'abort_on_error=1:halt_on_error=1:symbolize=1:log_path=\"{rundir}\\asan\":'
                                f'detect_stack_use_after_return=0:allocator_may_return_null=1:print_summary=1:'
                                f'handle_abort=1:windows_hook_rtl_allocators=0'})
    env.pop('PRINTBOT_MCP_ALLOW_PRINTER_CONTROL', None)
    return env

def x_action(run, pid, modal):
    tops = top_windows(pid)
    if not tops: time.sleep(0.5); return
    # native file pickers (IFileDialog: #32770 hosting shell views) -> type a path or cancel
    for t in tops:
        if cls(t) != '#32770': continue
        kids = children(t); kc = {cls(c) for c in kids}
        if kc & {'DirectUIHWND', 'SHELLDLL_DefView', 'ComboBoxEx32'}:
            edits = [c for c in kids if cls(c) == 'Edit']
            if edits and random.random() < 0.7:
                tt = title(t); save = bool(re.search(r'save|export', tt, re.I))
                p = os.path.join(SB_OUT, random.choice(['proj.3mf', 'g.gcode', 's.gcode.3mf', 'x.stl', 'z'])) if save \
                    else os.path.join(CORPUS, random.choice(os.listdir(CORPUS)))
                buf = ctypes.create_unicode_buffer(p); r = ctypes.c_size_t()
                u32.SendMessageTimeoutW(edits[0], WM_SETTEXT, 0, ctypes.cast(buf, ctypes.c_void_p).value, 2, 2000, ctypes.byref(r))
                post(t, WM_COMMAND, 1, 0, pid); run.act('fd_choose', title=tt, path=p)
            else:
                post(t, WM_COMMAND, 2, 0, pid); run.act('fd_cancel', title=title(t))
            stats['ui_actions'] += 1; return
    # wx dialogs (also #32770 on MSW): often press one of their labelled buttons/controls directly
    if cls(tops[0]) == '#32770' and random.random() < 0.45:
        t = tops[0]
        cands = [c for c in children(t) if u32.IsWindowVisible(c) and u32.IsWindowEnabled(c) and title(c)
                 and cls(c) in ('wxWindow', 'Button', 'wxWindowNR') and title(c) not in ('panel', 'control')]
        if cands:
            c = random.choice(cands); x, y, cw, ch = rect(c)
            mouse(pid, t, x + cw // 2, y + ch // 2, 'click'); run.act('dlg_press', title=title(t), ctl=title(c))
            stats['ui_actions'] += 1; return
    w = tops[0] if random.random() < 0.7 else random.choice(tops)   # EnumWindows is z-order: [0] is topmost
    X, Y, Wd, H = rect(w)
    if Wd < 4 or H < 4: return
    def rp():
        if random.random() < 0.4:
            return random.choice([(X + random.randint(0, Wd - 1), Y + random.randint(0, min(H - 1, 90))),
                                  (X + random.randint(max(0, Wd - 420), Wd - 1), Y + random.randint(0, H - 1)),
                                  (X + random.randint(0, Wd - 1), Y + random.randint(max(0, H - 60), H - 1))])
        return X + random.randint(0, Wd - 1), Y + random.randint(0, H - 1)
    r = random.random(); mods = random.choice([0, 0, 0, MK_SHIFT, MK_CTRL])
    if r < 0.40:
        x, y = rp(); b = random.choices([1, 2, 3], [0.8, 0.17, 0.03])[0]
        h = mouse(pid, w, x, y, 'click', b, mods); run.act('click', w=cls(w), x=x - X, y=y - Y, b=b, mods=mods, tgt=cls(h))
    elif r < 0.47:
        x, y = rp(); h = mouse(pid, w, x, y, 'dclick'); run.act('dclick', w=cls(w), x=x - X, y=y - Y, tgt=cls(h))
    elif r < 0.60:
        a, b = rp(), rp(); btn = random.choice([1, 1, 2, 3])
        h = drag(pid, w, a, b, btn, mods); run.act('drag', w=cls(w), a=a, b=b, btn=btn, mods=mods, tgt=cls(h))
    elif r < 0.68:
        x, y = rp(); n = random.randint(1, 8); h = wheel(pid, w, x, y, n, mods); run.act('wheel', x=x - X, y=y - Y, n=n, mods=mods, tgt=cls(h))
    elif r < 0.88:
        k = random.choice(list(VK.keys())); h = key(pid, w, k); run.act('key', key=k, tgt=cls(h))
    elif r < 0.93:
        txt = random.choice(['0', '-1', '999999', 'abc', '0.0001', '1e10', '%', '50%', '\u00e9\u4e2d', '..', '12.5', ''])
        h = type_text(pid, w, txt); key(pid, w, random.choice(['Return', 'Tab'])); run.act('type', text=txt, tgt=cls(h))
    elif r < 0.97:
        nw, nh = random.choice([(200, 150), (640, 480), (1024, 700), (1920, 1080), (1400, 900), (90, 60), (3000, 2000)])
        if ours(w, pid): u32.SetWindowPos(w, None, 0, 0, nw, nh, 0x0002 | 0x0004 | 0x0010)  # NOMOVE|NOZORDER|NOACTIVATE
        run.act('resize', w=cls(w), size=(nw, nh))
    else:
        nx, ny = random.randint(0, 2000), random.randint(0, 1200)
        if ours(w, pid): u32.SetWindowPos(w, None, nx, ny, 0, 0, 0x0001 | 0x0004 | 0x0010)
        run.act('move', w=cls(w), pos=(nx, ny))
    stats['ui_actions'] += 1

def modal_open(pid):
    tops = top_windows(pid)
    return len(tops) > 1 and any(not u32.IsWindowEnabled(t) for t in tops)

def mcp_loop(run, mcp, stop, pid):
    tools = [t for t in mcp.tools if t['name'] not in BLOCKED_TOOLS and not BLOCK_RE.search(t['name'])]
    weights = []
    for t in tools:
        n = t['name']; wgt = 1.0
        if n in ('load_model', 'open_project', 'slice', 'ui_switch_tab', 'ui_open_dialog', 'ui_dialog_click',
                 'ui_dialog_set', 'undo', 'redo', 'transform_object', 'select_presets', 'set_preset_settings'): wgt = 3.0
        if n in ('ui_screenshot', 'get_events', 'get_version'): wgt = 0.3
        weights.append(wgt)
    while not stop.is_set():
        t = random.choices(tools, weights)[0]
        mutating = not READONLY_RE.search(t['name']) and not t['name'].startswith('ui_dialog')
        if mutating and modal_open(pid) and random.random() > args.modal_mcp_prob:
            time.sleep(0.3); continue
        a = mcp_args(t)
        if t['name'] in ('ui_dialog_click', 'ui_dialog_set', 'ui_dialog_inspect', 'ui_close_dialog') and random.random() < 0.7:
            try:
                d = mcp.call('tools/call', {'name': 'ui_list_dialogs', 'arguments': {}}, timeout=30)
                ids = re.findall(r'\\"(?:id|dialog_id|dialog)\\":\s*\\?"?([\w\-:.]+)', json.dumps(d))
                if ids: a['dialog'] = a['dialog_id'] = random.choice(ids)
            except Exception: pass
        run.act('mcp', tool=t['name'], args=a)
        try:
            r = mcp.call('tools/call', {'name': t['name'], 'arguments': a}, timeout=180)
            stats['mcp_calls'] += 1
            res = r.get('result', {}); txt = json.dumps(r)
            if r.get('error') or res.get('isError'): stats['mcp_errors'] += 1
            if 'ui.busy' in txt or 'blocked_by_dialog' in txt: stats['mcp_busy'] += 1
            run.act('mcp_res', tool=t['name'], err=bool(r.get('error') or res.get('isError')), txt=txt[:300])
        except TimeoutError:
            run.act('mcp_timeout', tool=t['name']); stats['mcp_errors'] += 1
        except Exception as e:
            run.act('mcp_dead', err=str(e)); return
        time.sleep(random.uniform(0.0, 1.5))

FRAME_RE = re.compile(r'#\d+ 0x[0-9a-f]+ in (.+?) (\S+?\.\w+:\d+)', re.M)
def sig_of(text):
    m = re.search(r'ERROR: AddressSanitizer: ([\w\-]+)', text)
    frames = [(re.sub(r'\(.*', '', f)[-90:], l.replace(REPO + '\\', '')) for f, l in FRAME_RE.findall(text)]
    proj = [(f, l) for f, l in frames if re.search(r'(^|\\)src\\', l)][:3] or frames[:3]
    kind = 'ASan ' + m.group(1) if m else 'crash'
    return kind, proj, kind + '|' + '|'.join(f for f, _ in proj)

def main():
    deadline = time.time() + args.minutes * 60
    uniq = json.load(open(os.path.join(CRASHES, 'uniq.json'))) if os.path.exists(os.path.join(CRASHES, 'uniq.json')) else {}
    idx = len(glob.glob(os.path.join(OUT, 'run*')))
    log('START', args)
    while time.time() < deadline:
        idx += 1; stats['runs'] += 1
        fresh = (not os.path.isdir(SEED)) or random.random() < args.fresh_prob
        data = setup_sandbox(fresh)
        run = Run(idx); env = app_env(run.dir)
        applog = open(os.path.join(run.dir, 'app.log'), 'w')
        p = subprocess.Popen([APP, '--datadir', data], env=env, stdout=applog, stderr=subprocess.STDOUT, cwd=SB_OUT)
        log(f'run {idx} pid {p.pid} fresh={fresh}'); run.act('start', pid=p.pid, fresh=fresh)
        mcp = None; stop = threading.Event(); last_shot = 0; started = time.time(); hung_since = None; hang = False
        budget = random.uniform(300, 600)
        while p.poll() is None and time.time() < deadline and time.time() - started < budget:
            if mcp is None and os.path.exists(env['PRINTBOT_MCP_SOCKET']):
                try:
                    mcp = Mcp(env['PRINTBOT_MCP_SOCKET'], open(os.path.join(run.dir, 'mcp.log'), 'w'))
                    mcp.call('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                            'clientInfo': {'name': 'fuzz', 'version': '1'}}, timeout=60)
                    mcp.notify('notifications/initialized')
                    mcp.tools = mcp.call('tools/list', {}, timeout=60)['result']['tools']
                    run.act('mcp_ready', n=len(mcp.tools))
                    threading.Thread(target=mcp_loop, args=(run, mcp, stop, p.pid), daemon=True).start()
                except Exception as e:
                    run.act('mcp_init_fail', err=str(e)); log('mcp init fail', e)
                    if mcp: mcp.close()
                    mcp = None if time.time() - started < 120 else False
            tops = top_windows(p.pid)
            if tops and any(u32.IsHungAppWindow(t) for t in tops):
                hung_since = hung_since or time.time()
                if time.time() - hung_since > 120: hang = True; break
            else: hung_since = None
            try: x_action(run, p.pid, False)
            except Exception as e: run.act('x_err', err=repr(e))
            time.sleep(random.uniform(0.05, 0.6))
            if time.time() - last_shot > 60 and tops:
                last_shot = time.time(); screenshot(tops[-1], os.path.join(run.dir, 'shot_%d.png' % int(time.time() - started)))
        stop.set()
        rc = p.poll()
        tops = top_windows(p.pid)
        if rc is None:
            if tops: screenshot(tops[-1], os.path.join(run.dir, 'final.png'))
            if hang:
                stats['hangs'] += 1; log('HANG in run', idx)
                subprocess.run(['powershell', '-NoProfile', '-Command', f'Get-Process -Id {p.pid} | Select-Object Threads | Out-String'],
                               capture_output=True)
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
            try: p.wait(30)
            except Exception: pass
            rc = 'hang' if hang else 'killed'
        if mcp: mcp.close()
        time.sleep(1)
        reports = [(f, open(f, errors='replace').read()) for f in glob.glob(os.path.join(run.dir, 'asan*'))]
        log(f'run {idx} ended rc={rc} reports={len(reports)} ui={stats["ui_actions"]} mcp={stats["mcp_calls"]}')
        run.act('end', rc=rc)
        crashed = rc not in (0, 'killed', 'hang')
        if crashed: stats['crashes'] += 1
        elif rc == 0: stats['clean_exits'] += 1
        for f, text in reports:
            kind, frames, key = sig_of(text)
            if key not in uniq:
                cd = os.path.join(CRASHES, 'c%03d' % (len(uniq) + 1)); os.makedirs(cd, exist_ok=True)
                open(os.path.join(cd, 'report.txt'), 'w').write(text)
                with open(os.path.join(cd, 'actions_before.jsonl'), 'w') as fo:
                    for rec in run.alog: fo.write(json.dumps(rec, default=str) + '\n')
                for s_ in sorted(glob.glob(os.path.join(run.dir, '*.png')), key=os.path.getmtime)[-2:]: shutil.copy(s_, cd)
                uniq[key] = {'dir': cd, 'kind': kind, 'frames': frames, 'count': 0, 'runs': []}
                log('NEW finding', kind, frames[:2])
            uniq[key]['count'] += 1; uniq[key]['runs'].append(idx)
        if (crashed or rc == 'hang') and not reports:
            key = f'noreport|rc={rc}'
            if key not in uniq:
                cd = os.path.join(CRASHES, 'c%03d' % (len(uniq) + 1)); os.makedirs(cd, exist_ok=True)
                open(os.path.join(cd, 'report.txt'), 'w').write(open(os.path.join(run.dir, 'app.log'), errors='replace').read()[-8000:])
                with open(os.path.join(cd, 'actions_before.jsonl'), 'w') as fo:
                    for rec in run.alog: fo.write(json.dumps(rec, default=str) + '\n')
                uniq[key] = {'dir': cd, 'kind': f'exit rc={rc} no ASan report', 'frames': [], 'count': 0, 'runs': []}
            uniq[key]['count'] += 1; uniq[key]['runs'].append(idx)
        if not os.path.isdir(SEED) and os.path.exists(os.path.join(data, 'BambuStudio.conf')) and rc in ('killed', 0):
            shutil.copytree(data, SEED, ignore=shutil.ignore_patterns('log', 'cache', '*.lock'))
            log('seeded data dir from run', idx)
        json.dump(uniq, open(os.path.join(CRASHES, 'uniq.json'), 'w'), indent=1, default=str)
        json.dump(stats, open(os.path.join(WORK, 'stats_' + args.tag + '.json'), 'w'), indent=1)
    json.dump(stats, open(os.path.join(WORK, 'stats_' + args.tag + '.json'), 'w'), indent=1)
    log('DONE', stats)

main()
