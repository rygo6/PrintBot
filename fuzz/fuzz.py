#!/usr/bin/env python3
"""PrintBot chaos GUI + MCP fuzzer for Linux (ASan/UBSan build, Xvfb, no-network namespace).
See README.md. Paths come from fuzzcfg.py (repo-relative defaults, env/flag overrides)."""
import os, sys, time, json, random, subprocess, threading, signal, re, glob, shutil, argparse, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzcfg as cfg

ap = argparse.ArgumentParser(description='Chaos GUI + MCP fuzzer for the PrintBot sanitizer build (Linux, Xvfb).')
ap.add_argument('--minutes', type=float, default=60, help='total campaign length (default 60)')
ap.add_argument('--leaks', action='store_true', help='leak mode: detect_leaks=1, shorter runs, LSan check at the end of each run')
ap.add_argument('--tag', default='main', help='name for this campaign; outputs go to <work>/runs_<tag>, crashes_<tag>, stats_<tag>.json')
ap.add_argument('--fresh-prob', type=float, default=0.15, help='chance a run starts from an empty profile instead of the seed profile')
ap.add_argument('--app', default=cfg.APP, help='sanitizer GUI binary (env PRINTBOT_FUZZ_APP)')
ap.add_argument('--mcp', default=cfg.MCPBIN, help='print_bot-mcp relay binary (env PRINTBOT_FUZZ_MCP)')
ap.add_argument('--work', default=cfg.WORK, help='output directory (env PRINTBOT_FUZZ_WORK, default <repo>/temp/fuzz)')
ap.add_argument('--corpus', default=None, help='input corpus (env PRINTBOT_FUZZ_CORPUS, default <work>/corpus)')
ap.add_argument('--display', default=cfg.DISP, help='Xvfb display (env PRINTBOT_FUZZ_DISPLAY, default :77)')
args = ap.parse_args()

FZ = os.path.abspath(args.work); os.makedirs(FZ, exist_ok=True)
REPO = cfg.REPO
APP, MCPBIN, DISP = os.path.abspath(args.app), os.path.abspath(args.mcp), args.display
cfg.WORK = FZ; cfg.DISP = DISP
CORPUS = os.path.abspath(args.corpus or os.environ.get('PRINTBOT_FUZZ_CORPUS') or os.path.join(FZ, 'corpus'))
BLOCKED_TOOLS = cfg.BLOCKED_TOOLS
for _p, _what in [(APP, '--app'), (MCPBIN, '--mcp')]:
    if not os.path.exists(_p): sys.exit(f'{_what}: {_p} does not exist (build it or pass the path)')
if not os.path.isdir(CORPUS) or not os.listdir(CORPUS):
    sys.exit(f'corpus {CORPUS} is empty: run gen_corpus.py first')

OUT = os.path.join(FZ, 'runs_' + args.tag)
os.makedirs(OUT, exist_ok=True)
CRASHES = os.path.join(FZ, 'crashes_' + args.tag)
os.makedirs(CRASHES, exist_ok=True)
SEED = os.path.join(FZ, 'seed_home')
stats = {'runs': 0, 'x_actions': 0, 'mcp_calls': 0, 'mcp_errors': 0, 'crashes': 0, 'hangs': 0, 'clean_exits': 0,
         'start': time.time()}

def log(*a):
    s = time.strftime('%H:%M:%S') + ' ' + ' '.join(str(x) for x in a)
    print(s, flush=True)

def xdo(*a, timeout=8):
    env = cfg.x_env()
    try:
        r = subprocess.run([cfg.xdotool(), *map(str, a)], env=env, capture_output=True,
                           text=True, timeout=timeout)
        return r.stdout.strip()
    except subprocess.TimeoutExpired:
        return ''

def screenshot(path):
    env = dict(os.environ, DISPLAY=DISP, GDK_BACKEND='x11'); env.pop('WAYLAND_DISPLAY', None)
    env.pop('PYTHONHOME', None); env.pop('PYTHONPATH', None)
    try:
        root = subprocess.run(['xwininfo', '-root'], env=env, capture_output=True, text=True, timeout=5).stdout
        wid = re.search(r'Window id: (0x[0-9a-f]+)', root).group(1)
        subprocess.run(['python3', os.path.join(cfg.FZ, 'shot.py'), wid, path], env=env,
                       capture_output=True, timeout=15)
    except Exception as e:
        log('screenshot failed', e)

def win_info(w):
    env = dict(os.environ, DISPLAY=DISP)
    try:
        r = subprocess.run(['xprop', '-id', w, 'WM_WINDOW_ROLE', 'WM_NAME', '_NET_WM_WINDOW_TYPE'], env=env,
                           capture_output=True, text=True, timeout=5).stdout
    except Exception:
        r = ''
    return r

class Mcp:
    def __init__(self, sock, logf):
        self.p = subprocess.Popen([MCPBIN, '--resources', os.path.join(REPO, 'resources'), '--gui', '--socket', sock,
                                   '--wait', '90'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=logf, text=True, bufsize=1)
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
    return random.choice(['', 'a', 'Cube', 'x' * random.randint(1, 3000), '\u00e9\u4e2d\U0001F600', '../..', '%s%n%x',
                          '-1', 'nan', '1e309', ' ', '\n\t', 'true', 'null', '0', 'PLA', 'Bambu PLA Basic @BBL X1C',
                          'Bambu Lab X1 Carbon 0.4 nozzle', '0.20mm Standard @BBL X1C', 'Textured PEI Plate',
                          'by_layer', 'by_object', 'layer_height', 'sparse_infill_density', 'wall_loops'])

def rand_path(key):
    files = os.listdir(CORPUS)
    if random.random() < 0.6 and not re.search(r'out|export|save|dest', key, re.I):
        return os.path.join(CORPUS, random.choice(files))
    return os.path.join(SB_OUT, random.choice(['p.3mf', 'o.gcode', 'x.gcode.3mf', 'y.png', 'sub/dir/z.3mf', 'q', '']))

def rand_value(schema, key='', depth=0):
    if not isinstance(schema, dict): schema = {}
    if random.random() < 0.06 or depth > 4:
        return random.choice([None, -1, 0, 1, 2**63, -2**31, 1e308, float('nan') if False else 3.5, '', 'zz', [], {}, True])
    if 'enum' in schema and random.random() < 0.85: return random.choice(schema['enum'])
    t = schema.get('type')
    if isinstance(t, list): t = random.choice(t)
    if t is None and 'anyOf' in schema: return rand_value(random.choice(schema['anyOf']), key, depth + 1)
    if t is None and 'oneOf' in schema: return rand_value(random.choice(schema['oneOf']), key, depth + 1)
    if t == 'string':
        if PATH_KEYS.search(key): return rand_path(key)
        return rand_string()
    if t == 'integer':
        return random.choice([0, 1, 1, 2, 3, -1, 5, 100, 2**31, random.randint(-5, 20)])
    if t == 'number':
        return random.choice([0, 0.1, 1, 2.5, -10, 45, 90, 180, 1000, 1e9, -1e-9, random.uniform(-200, 200)])
    if t == 'boolean': return random.random() < 0.5
    if t == 'array':
        it = schema.get('items', {})
        return [rand_value(it, key, depth + 1) for _ in range(random.choice([0, 1, 1, 2, 3, 8]))]
    if t == 'object' or 'properties' in schema:
        props = schema.get('properties', {}); req = set(schema.get('required', []))
        o = {}
        for k, s in props.items():
            if k in req or random.random() < 0.5: o[k] = rand_value(s, k, depth + 1)
        if random.random() < 0.05: o['bogus_' + str(random.randint(0, 9))] = 1
        if not props and random.random() < 0.5:   # free-form settings maps
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
        for k in list((schema.get('properties') or {}).keys()):
            if PATH_KEYS.search(k):
                v = schema['properties'][k]
                a[k] = [os.path.join(CORPUS, random.choice(fs)) for _ in range(random.randint(1, 3))] \
                    if v.get('type') == 'array' else os.path.join(CORPUS, random.choice(fs))
    return a

KEYS = ['Escape', 'Return', 'Tab', 'shift+Tab', 'Delete', 'BackSpace', 'space', 'Up', 'Down', 'Left', 'Right',
        'Page_Up', 'Page_Down', 'Home', 'End', 'ctrl+z', 'ctrl+y', 'ctrl+shift+z', 'ctrl+a', 'ctrl+c', 'ctrl+v',
        'ctrl+x', 'ctrl+k', 'ctrl+l', 'ctrl+e', 'ctrl+r', 'ctrl+g', 'ctrl+d', 'ctrl+n', 'ctrl+o', 'ctrl+i',
        'ctrl+s', 'ctrl+shift+s', 'ctrl+shift+g', 'ctrl+comma', 'ctrl+t', 'ctrl+m', 'ctrl+p', 'ctrl+1', 'ctrl+2',
        'ctrl+3', 'ctrl+4', 'ctrl+5', 'ctrl+6', 'ctrl+0', '0', '1', '2', '3', '4', '5', '6', 'a', 'b', 'c', 'e',
        'f', 'h', 'l', 'm', 'o', 'p', 'r', 's', 't', 'u', 'w', 'x', 'z', 'plus', 'minus', 'question', 'slash',
        'alt+f', 'alt+e', 'alt+v', 'alt+h', 'F1', 'F2', 'F5', 'F10', 'F11', 'shift+Delete', 'ctrl+Delete',
        'ctrl+shift+Escape', 'alt+F4', 'ctrl+q', 'ctrl+w', 'shift+a', 'shift+r', 'shift+F', 'ctrl+alt+s']
RARE_KEYS = {'ctrl+q': 0.05, 'alt+F4': 0.15, 'ctrl+w': 0.2}
# Phase 3: the X fuzzer pauses while the MCP thread clears dialogs and lands a state-changing call.
X_PAUSE = threading.Event()
NEST_OK = {'ui_list_dialogs', 'ui_dialog_inspect', 'ui_dialog_click', 'ui_dialog_set', 'ui_close_dialog', 'ui_screenshot'}
def is_mutating(t):
    ann = t.get('annotations') or {}
    if 'readOnlyHint' in ann: ro = bool(ann['readOnlyHint'])
    else: ro = t['name'].startswith(('get_', 'list_', 'ui_list', 'describe'))
    return not ro and t['name'] not in NEST_OK

class Run:
    def __init__(self, idx):
        self.idx = idx
        self.dir = os.path.join(OUT, 'run%03d' % idx); os.makedirs(self.dir, exist_ok=True)
        self.actions = open(os.path.join(self.dir, 'actions.jsonl'), 'w')
        self.alog = []
        self.lock = threading.Lock()
    def act(self, kind, **kw):
        rec = {'t': round(time.time() - self.t0, 2), 'k': kind, **kw}
        with self.lock:
            self.alog.append(rec); self.alog = self.alog[-400:]
            self.actions.write(json.dumps(rec, default=str) + '\n'); self.actions.flush()

def setup_home(fresh):
    global SB, SB_OUT
    SB = os.path.join(FZ, 'sb_' + args.tag)
    if os.path.isdir(SB): shutil.rmtree(SB, ignore_errors=True)
    os.makedirs(SB)
    home = os.path.join(SB, 'home')
    if not fresh and os.path.isdir(SEED): shutil.copytree(SEED, home, symlinks=True)
    os.makedirs(home, exist_ok=True)
    for d in ['run', 'out', 'out/sub', 'tmp']: os.makedirs(os.path.join(SB, d), exist_ok=True)
    os.chmod(os.path.join(SB, 'run'), 0o700)
    SB_OUT = os.path.join(SB, 'out')
    return home

def app_env(home, rundir):
    env = {k: v for k, v in os.environ.items() if k not in ('WAYLAND_DISPLAY', 'PYTHONHOME', 'PYTHONPATH',
            'LD_LIBRARY_PATH', 'APPDIR', 'APPIMAGE', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER',
            'XDG_SESSION_TYPE', 'GNOME_SETUP_DISPLAY', 'XAUTHORITY', 'SSH_AUTH_SOCK')}
    leaks = 1 if args.leaks else 0
    env.update({
        'DISPLAY': DISP, 'GDK_BACKEND': 'x11', 'HOME': home,
        'XDG_CONFIG_HOME': os.path.join(home, '.config'), 'XDG_DATA_HOME': os.path.join(home, '.local/share'),
        'XDG_CACHE_HOME': os.path.join(home, '.cache'), 'XDG_RUNTIME_DIR': os.path.join(SB, 'run'),
        'TMPDIR': os.path.join(SB, 'tmp'), 'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent',
        'NO_AT_BRIDGE': '1', 'GTK_A11Y': 'none', 'BROWSER': os.path.join(cfg.stub_bin_dir(), 'xdg-open'),
        'PATH': cfg.stub_bin_dir() + ':' + env.get('PATH', '/usr/bin:/bin'),
        'PRINTBOT_MCP_SOCKET': os.path.join(SB, 'run', 'mcp.sock'), 'FUZZ_SB': SB,
        'SSL_CERT_FILE': cfg.SSL_CERT_FILE,
        'ASAN_SYMBOLIZER_PATH': cfg.symbolizer(),
        'ASAN_OPTIONS': f'detect_leaks={leaks}:abort_on_error=1:halt_on_error=1:symbolize=1:'
                        f'log_path={rundir}/asan:detect_stack_use_after_return=0:allocator_may_return_null=1:'
                        f'handle_abort=1:handle_segv=1:print_summary=1',
        'UBSAN_OPTIONS': f'print_stacktrace=1:log_path={rundir}/ubsan:halt_on_error=0:suppressions={cfg.SUPP}',
        'LSAN_OPTIONS': f'log_path={rundir}/lsan',
    })
    if args.leaks:
        env['LD_PRELOAD'] = cfg.lsan_shim()
        env['FUZZ_LEAKCHECK_FILE'] = os.path.join(rundir, 'do_leakcheck')
    return env

def find_windows(pid):
    out = xdo('search', '--onlyvisible', '--pid', pid)
    ws = [w for w in out.split() if w]
    return ws

def geom(w):
    o = xdo('getwindowgeometry', '--shell', w)
    d = dict(l.split('=', 1) for l in o.splitlines() if '=' in l)
    try: return int(d['X']), int(d['Y']), int(d['WIDTH']), int(d['HEIGHT'])
    except Exception: return None

def handle_file_chooser(run, w, info):
    save = bool(re.search(r'save|export', info, re.I))
    if random.random() < 0.25:
        xdo('key', '--window', w, 'Escape'); run.act('fc_escape', w=w); return
    if save:
        p = os.path.join(SB_OUT, random.choice(['proj.3mf', 'g.gcode', 's.gcode.3mf', 'x.stl', 'y.obj', 'z']))
    else:
        p = os.path.join(CORPUS, random.choice(os.listdir(CORPUS)))
    xdo('windowfocus', w); xdo('key', '--window', w, 'ctrl+l'); time.sleep(0.2)
    xdo('type', '--window', w, '--delay', '5', p); time.sleep(0.2)
    xdo('key', '--window', w, 'Return'); run.act('fc_choose', w=w, path=p, save=save)
    time.sleep(0.5)
    # gtk overwrite-confirm sub dialog -> Return
    xdo('key', 'Return')

def x_action(run, pid):
    if X_PAUSE.is_set():
        time.sleep(0.1); return
    ws = find_windows(pid)
    if not ws: time.sleep(0.5); return
    infos = {w: win_info(w) for w in ws[-4:]}
    for w, info in infos.items():
        if 'GtkFileChooserDialog' in info:
            handle_file_chooser(run, w, info); stats['x_actions'] += 1; return
    # prefer the newest (likely dialog/popup)
    w = ws[-1] if random.random() < 0.7 else random.choice(ws)
    g = geom(w)
    if not g or g[2] < 4 or g[3] < 4: return
    X, Y, W, H = g
    def rp():
        # bias toward edges (toolbars/sidebars/tabs) of the main window
        if random.random() < 0.4:
            return random.choice([(random.randint(0, W - 1), random.randint(0, min(H - 1, 90))),
                                  (random.randint(max(0, W - 420), W - 1), random.randint(0, H - 1)),
                                  (random.randint(0, W - 1), random.randint(max(0, H - 60), H - 1))])
        return random.randint(0, W - 1), random.randint(0, H - 1)
    r = random.random()
    if r < 0.38:
        x, y = rp(); b = random.choices([1, 3, 2], [0.8, 0.17, 0.03])[0]
        xdo('mousemove', '--window', w, x, y, 'click', b); run.act('click', w=w, x=x, y=y, b=b)
    elif r < 0.45:
        x, y = rp(); xdo('mousemove', '--window', w, x, y, 'click', '--repeat', 2, '--delay', 60, 1)
        run.act('dclick', w=w, x=x, y=y)
    elif r < 0.57:
        x, y = rp(); x2, y2 = rp(); b = random.choice([1, 1, 3, 2])
        mods = random.choice([None, None, 'shift', 'ctrl', 'alt'])
        if mods: xdo('keydown', mods)
        xdo('mousemove', '--window', w, x, y, 'mousedown', b)
        for i in range(1, 6):
            xdo('mousemove', '--window', w, x + (x2 - x) * i // 5, y + (y2 - y) * i // 5); time.sleep(0.02)
        xdo('mouseup', b)
        if mods: xdo('keyup', mods)
        run.act('drag', w=w, a=(x, y), b=(x2, y2), btn=b, mods=mods)
    elif r < 0.65:
        x, y = rp(); b = random.choice([4, 5, 6, 7]); n = random.randint(1, 8)
        mods = random.choice([None, None, 'ctrl', 'shift'])
        if mods: xdo('keydown', mods)
        xdo('mousemove', '--window', w, x, y, 'click', '--repeat', n, '--delay', 20, b)
        if mods: xdo('keyup', mods)
        run.act('scroll', w=w, x=x, y=y, b=b, n=n, mods=mods)
    elif r < 0.88:
        k = random.choice(KEYS)
        if k in RARE_KEYS and random.random() > RARE_KEYS[k]: k = 'Escape'
        x, y = rp(); xdo('mousemove', '--window', w, x, y)
        xdo('windowfocus', w); xdo('key', '--clearmodifiers', k); run.act('key', w=w, key=k, x=x, y=y)
    elif r < 0.93:
        txt = random.choice(['0', '-1', '999999', 'abc', '0.0001', '1e10', '%', '50%', '\u00e9\u4e2d', '..', '12.5', ''])
        xdo('windowfocus', w); xdo('type', '--delay', '10', txt); xdo('key', random.choice(['Return', 'Tab']))
        run.act('type', w=w, text=txt)
    elif r < 0.97:
        nw, nh = random.choice([(200, 150), (640, 480), (1024, 700), (1920, 1080), (1400, 900), (90, 60), (3000, 2000)])
        xdo('windowsize', w, nw, nh); run.act('resize', w=w, size=(nw, nh))
    else:
        nx, ny = random.randint(-200, 1500), random.randint(-100, 900)
        xdo('windowmove', w, nx, ny); run.act('move', w=w, pos=(nx, ny))
    stats['x_actions'] += 1

def mcp_loop(run, mcp, stop):
    tools = [t for t in mcp.tools if cfg.tool_allowed(t['name'])]
    weights = []
    for t in tools:
        n = t['name']
        wgt = 1.0
        if n in ('load_model', 'open_project', 'slice', 'ui_switch_tab', 'ui_open_dialog', 'ui_dialog_click',
                 'ui_dialog_set', 'undo', 'redo', 'transform_object', 'select_presets', 'set_preset_settings'):
            wgt = 3.0
        if n in ('ui_screenshot', 'get_events', 'get_version'): wgt = 0.3
        weights.append(wgt)
    while not stop.is_set():
        t = random.choices(tools, weights)[0]
        a = mcp_args(t)
        # dialogs: if a dialog is listed, poke it with real ids sometimes
        if t['name'] in ('ui_dialog_click', 'ui_dialog_set', 'ui_dialog_inspect', 'ui_close_dialog') and random.random() < 0.7:
            try:
                d = mcp.call('tools/call', {'name': 'ui_list_dialogs', 'arguments': {}}, timeout=30)
                txt = json.dumps(d)
                ids = re.findall(r'\\"(?:id|dialog_id|dialog)\\":\s*\\?"?([\w\-:.]+)', txt)
                if ids: a['dialog'] = a['dialog_id'] = random.choice(ids)
            except Exception: pass
        paused = False
        if is_mutating(t):
            # Mostly land state-changing calls with no modal open; ~15% still go in during a modal to test the guard.
            if random.random() < 0.85:
                X_PAUSE.set(); paused = True
                for _ in range(6):
                    try:
                        d = mcp.call('tools/call', {'name': 'ui_list_dialogs', 'arguments': {}}, timeout=30)
                        try: dl = json.loads(d['result']['content'][0]['text']).get('dialogs', [])
                        except Exception: dl = []
                        if not dl:
                            break
                        mcp.call('tools/call', {'name': 'ui_close_dialog', 'arguments': {}}, timeout=30)
                        stats['dialogs_closed'] = stats.get('dialogs_closed', 0) + 1
                        time.sleep(0.3)
                    except Exception:
                        break
                stats['mutating_cleared'] = stats.get('mutating_cleared', 0) + 1
            else:
                stats['mutating_during_modal_ok'] = stats.get('mutating_during_modal_ok', 0) + 1
        run.act('mcp', tool=t['name'], args=a, paused=paused)
        try:
            r = mcp.call('tools/call', {'name': t['name'], 'arguments': a}, timeout=180)
            txt_ = json.dumps(r)
            if 'ui.blocked_by_dialog' in txt_ or 'ui.busy' in txt_:
                stats['mcp_guard_refusals'] = stats.get('mcp_guard_refusals', 0) + 1
            stats['mcp_calls'] += 1
            res = r.get('result', {})
            if r.get('error') or res.get('isError'): stats['mcp_errors'] += 1
            run.act('mcp_res', tool=t['name'], err=bool(r.get('error') or res.get('isError')),
                    txt=json.dumps(r)[:300])
        except TimeoutError:
            run.act('mcp_timeout', tool=t['name']); stats['mcp_errors'] += 1
        except Exception as e:
            X_PAUSE.clear(); run.act('mcp_dead', err=str(e)); return
        finally:
            if paused:
                time.sleep(random.uniform(0.3, 1.5))  # let any result dialog appear before X resumes
                X_PAUSE.clear()
        time.sleep(random.uniform(0.0, 1.5))

def parse_report(rundir):
    txts = []
    for f in sorted(glob.glob(os.path.join(rundir, 'asan.*')) + glob.glob(os.path.join(rundir, 'ubsan.*'))):
        try: txts.append((f, open(f, errors='replace').read()))
        except Exception: pass
    return txts

FRAME_RE = re.compile(r'#\d+ 0x[0-9a-f]+ in (.+?) (/\S+?:\d+)(?::\d+)?\s*$', re.M)

def split_blocks(text):
    starts = [m.start() for m in re.finditer(r'(?m)^(?:\S+:\d+:\d+: runtime error:|=+\n==\d+==ERROR:|==\d+==ERROR:)', text)]
    if not starts: return [text] if text.strip() else []
    return [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]

def short_fn(f):
    f = re.sub(r'\(.*', '', f)
    return f[-90:]

def sig_of(text):
    m = re.search(r'ERROR: AddressSanitizer: ([\w\-]+)', text)
    frames = [(short_fn(f), l.replace(REPO + '/', '')) for f, l in FRAME_RE.findall(text)]
    proj = [(f, l) for f, l in frames if l.startswith('src/') and '/usr/' not in l][:3]
    if m:
        kind = 'ASan ' + m.group(1)
        if not proj: proj = frames[:3]
        key = kind + '|' + '|'.join(f for f, _ in proj)
        return kind, proj, key
    m = re.search(r'^(\S+?):(\d+):\d+: runtime error: (.*)$', text, re.M)
    if m:
        loc = m.group(1).replace(REPO + '/', '') + ':' + m.group(2)
        kind = 'UBSan ' + re.sub(r'0x[0-9a-f]+', 'ADDR', re.sub(r'value -?\d+', 'value N', m.group(3)))[:140]
        return kind, ([(loc, loc)] + proj)[:3], 'UB|' + loc
    m = re.search(r'ERROR: (\w+Sanitizer)[^\n]*', text)
    kind = m.group(0) if m else 'unknown'
    return kind, (proj or frames[:3]), kind + '|' + '|'.join(f for f, _ in (proj or frames[:3]))

def main():
    # The app runs with no network at all (unshare -c -n: user + network namespace, loopback only), so nothing it
    # does can reach a printer, cloud service or update server even if a tool filter were bypassed.
    if subprocess.run(['unshare', '-cn', 'true'], capture_output=True).returncode != 0:
        sys.exit('unshare -cn (unprivileged user + network namespace) is not available; refusing to run with network access')
    if not cfg.wait_display_free():
        sys.exit(f'display {DISP} is in use (stale /tmp/.X{DISP.lstrip(":")}-lock?); pick another with --display')
    # Xvfb
    xv = subprocess.Popen(['Xvfb', DISP, '-screen', '0', '1920x1080x24', '-nolisten', 'tcp', '+extension', 'GLX'],
                          stdout=open(os.path.join(FZ, 'xvfb_' + args.tag + '.log'), 'w'), stderr=subprocess.STDOUT)
    time.sleep(2)
    deadline = time.time() + args.minutes * 60
    uniq = {}
    if os.path.exists(os.path.join(CRASHES, 'uniq.json')):
        uniq = json.load(open(os.path.join(CRASHES, 'uniq.json')))
    idx = len(glob.glob(os.path.join(OUT, 'run*')))
    try:
        while time.time() < deadline:
            idx += 1; stats['runs'] += 1
            fresh = (not os.path.isdir(SEED)) or random.random() < args.fresh_prob
            home = setup_home(fresh)
            run = Run(idx); run.t0 = time.time()
            env = app_env(home, run.dir)
            applog = open(os.path.join(run.dir, 'app.log'), 'w')
            p = subprocess.Popen(['unshare', '-cn', APP], env=env, stdout=applog, stderr=subprocess.STDOUT,
                                 cwd=SB_OUT, start_new_session=True)
            log(f'run {idx} pid {p.pid} fresh={fresh}')
            run.act('start', pid=p.pid, fresh=fresh)
            mcp = None; stop = threading.Event(); th = None
            last_shot = 0; started = time.time(); hang = False
            run_budget = random.uniform(240, 300) if args.leaks else random.uniform(300, 600)
            while p.poll() is None and time.time() < deadline and time.time() - started < run_budget:
                if mcp is None and os.path.exists(env['PRINTBOT_MCP_SOCKET']):
                    try:
                        mcp = Mcp(env['PRINTBOT_MCP_SOCKET'], open(os.path.join(run.dir, 'mcp.log'), 'w'))
                        mcp.call('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                                'clientInfo': {'name': 'fuzz', 'version': '1'}}, timeout=60)
                        mcp.notify('notifications/initialized')
                        mcp.tools = mcp.call('tools/list', {}, timeout=60)['result']['tools']
                        run.act('mcp_ready', n=len(mcp.tools))
                        th = threading.Thread(target=mcp_loop, args=(run, mcp, stop), daemon=True); th.start()
                    except Exception as e:
                        run.act('mcp_init_fail', err=str(e)); log('mcp init fail', e)
                        if mcp: mcp.close()
                        mcp = None if time.time() - started < 120 else False
                try:
                    x_action(run, p.pid)
                except Exception as e:
                    run.act('x_err', err=repr(e))
                time.sleep(random.uniform(0.05, 0.6))
                if time.time() - last_shot > 60:
                    last_shot = time.time()
                    screenshot(os.path.join(run.dir, 'shot_%d.png' % int(time.time() - started)))
            stop.set(); X_PAUSE.clear()
            rc = p.poll()
            if rc is None:
                # end of budget: graceful-ish shutdown, then kill (only our own process group)
                screenshot(os.path.join(run.dir, 'final.png'))
                if args.leaks:
                    open(os.path.join(run.dir, 'do_leakcheck'), 'w').close()
                    try: p.wait(180)
                    except subprocess.TimeoutExpired: pass
                try:
                    if p.poll() is None: os.killpg(p.pid, signal.SIGTERM)
                except Exception: pass
                try: p.wait(20)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL); p.wait()
                rc = 'killed'
            else:
                screenshot(os.path.join(run.dir, 'after_exit.png'))
            if mcp: mcp.close()
            time.sleep(1)
            reports = parse_report(run.dir)
            log(f'run {idx} ended rc={rc} reports={len(reports)} x={stats["x_actions"]} mcp={stats["mcp_calls"]}')
            run.act('end', rc=rc)
            crashed = rc not in (0, 'killed')
            if crashed: stats['crashes'] += 1
            elif rc == 0: stats['clean_exits'] += 1
            for f, text in reports:
                for ch in split_blocks(text):
                    if not ch.strip(): continue
                    kind, frames, key = sig_of(ch)
                    fatal = kind.startswith('ASan') or 'Sanitizer' in kind
                    if key not in uniq:
                        cd = os.path.join(CRASHES, 'c%03d' % (len(uniq) + 1)); os.makedirs(cd, exist_ok=True)
                        open(os.path.join(cd, 'report.txt'), 'w').write(ch)
                        with open(os.path.join(cd, 'actions_before.jsonl'), 'w') as fo:
                            for rec in run.alog: fo.write(json.dumps(rec, default=str) + '\n')
                        if fatal:
                            for s_ in sorted(glob.glob(os.path.join(run.dir, '*.png')), key=os.path.getmtime)[-2:]: shutil.copy(s_, cd)
                        uniq[key] = {'dir': cd, 'kind': kind, 'frames': frames, 'count': 0, 'runs': [], 'first': f, 'fatal': fatal}
                        log('NEW finding', kind, frames[:2])
                    uniq[key]['count'] += 1
                    if idx not in uniq[key]['runs']: uniq[key]['runs'].append(idx)
            if crashed and not reports:
                key = f'noreport|rc={rc}'
                tail = open(os.path.join(run.dir, 'app.log'), errors='replace').read()[-6000:]
                if key not in uniq:
                    cd = os.path.join(CRASHES, 'c%02d' % (len(uniq) + 1)); os.makedirs(cd, exist_ok=True)
                    open(os.path.join(cd, 'report.txt'), 'w').write(tail)
                    with open(os.path.join(cd, 'actions_before.jsonl'), 'w') as fo:
                        for rec in run.alog: fo.write(json.dumps(rec, default=str) + '\n')
                    uniq[key] = {'dir': cd, 'kind': f'exit rc={rc} no sanitizer report', 'frames': [], 'count': 0, 'runs': []}
                uniq[key]['count'] += 1; uniq[key]['runs'].append(idx)
            # first healthy run with presets set up -> becomes seed
            if not os.path.isdir(SEED) and os.path.exists(os.path.join(home, '.config', 'PrintBot', 'BambuStudio.conf')) \
                    and rc in ('killed', 0):
                shutil.copytree(home, SEED, symlinks=True, ignore=shutil.ignore_patterns('log', 'cache', '*.lock'))
                log('seeded home from run', idx)
            json.dump(uniq, open(os.path.join(CRASHES, 'uniq.json'), 'w'), indent=1, default=str)
            json.dump(stats, open(os.path.join(FZ, 'stats_' + args.tag + '.json'), 'w'), indent=1)
    finally:
        xv.terminate()
        json.dump(stats, open(os.path.join(FZ, 'stats_' + args.tag + '.json'), 'w'), indent=1)
        log('DONE', stats)

main()
