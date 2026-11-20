#!/usr/bin/env python3
"""PrintBot fuzzer for macOS (Apple clang ASan+UBSan build).

The app is driven only through its MCP endpoint, including the UI tools (tabs, camera, dialogs: open, inspect,
set, click, close), with random schema-shaped and hostile arguments. No global mouse/keyboard events are
synthesized, so nothing can land in other apps and no Accessibility or Screen Recording permission is needed.

Isolation: a throwaway --datadir, HOME, TMPDIR and MCP socket per run, and the app runs under sandbox-exec
with all IP networking denied (local Unix sockets still work for MCP), so it cannot reach printers or cloud
services. Printer and print-job tools are filtered out (fuzzcfg.BLOCKED_TOOLS / BLOCK_RE).

  python3 fuzz/fuzz_mac.py --app build-asan/src/BambuStudio.app --minutes 45 --tag mac
"""
import os, sys, time, json, random, subprocess, threading, re, glob, shutil, argparse, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzcfg as cfg

ap = argparse.ArgumentParser(description='MCP-driven fuzzer for the macOS sanitizer build.')
ap.add_argument('--minutes', type=float, default=45)
ap.add_argument('--tag', default='mac')
ap.add_argument('--app', default=None, help='the .app bundle or its executable (default: first *.app under <repo>/build-asan/src)')
ap.add_argument('--mcp', default=None, help='print_bot-mcp relay (default: next to the app executable, else <repo>/build-asan/src/print_bot-mcp)')
ap.add_argument('--work', default=cfg.WORK)
ap.add_argument('--corpus', default=None)
ap.add_argument('--fresh-prob', type=float, default=0.2)
ap.add_argument('--modal-mcp-prob', type=float, default=0.15, help='chance a state-changing call is sent while a dialog is open')
ap.add_argument('--no-sandbox', action='store_true', help='do not wrap the app in sandbox-exec (NOT recommended: network allowed)')
args = ap.parse_args()

REPO = cfg.REPO
WORK = os.path.abspath(args.work); os.makedirs(WORK, exist_ok=True)
CORPUS = os.path.abspath(args.corpus or os.environ.get('PRINTBOT_FUZZ_CORPUS') or os.path.join(WORK, 'corpus'))

def find_app():
    a = args.app or os.environ.get('PRINTBOT_FUZZ_APP')
    if not a:
        c = sorted(glob.glob(os.path.join(REPO, 'build-asan', 'src', '*.app')))
        a = c[0] if c else ''
    if a.endswith('.app'):
        exe = glob.glob(os.path.join(a, 'Contents', 'MacOS', '*'))
        a = exe[0] if exe else a
    return os.path.abspath(a) if a else ''

APP = find_app()
MCPBIN = os.path.abspath(args.mcp or os.environ.get('PRINTBOT_FUZZ_MCP') or next(
    (p for p in [os.path.join(os.path.dirname(APP), 'print_bot-mcp'), os.path.join(REPO, 'build-asan', 'src', 'print_bot-mcp'),
                 os.path.join(REPO, 'build', 'src', 'print_bot-mcp')] if os.path.exists(p)), 'print_bot-mcp'))
for _p, _w in [(APP, '--app'), (MCPBIN, '--mcp')]:
    if not _p or not os.path.exists(_p): sys.exit(f'{_w}: {_p!r} does not exist (build it or pass the path)')
if not os.path.isdir(CORPUS) or not os.listdir(CORPUS): sys.exit(f'corpus {CORPUS} is empty: run gen_corpus.py first')

OUT = os.path.join(WORK, 'runs_' + args.tag); os.makedirs(OUT, exist_ok=True)
CRASHES = os.path.join(WORK, 'crashes_' + args.tag); os.makedirs(CRASHES, exist_ok=True)
SEED = os.path.join(WORK, 'seed_' + args.tag)
stats = {'runs': 0, 'mcp_calls': 0, 'mcp_errors': 0, 'mcp_guard_refusals': 0, 'dialogs_closed': 0, 'crashes': 0,
         'hangs': 0, 'clean_exits': 0, 'start': time.time()}
# Deny all IP networking; Unix-domain sockets (the MCP endpoint) and files stay allowed.
SANDBOX = '(version 1)(allow default)(deny network-outbound (remote ip))(deny network-inbound (local ip))(deny network-bind (local ip))'

def log(*a):
    s = time.strftime('%H:%M:%S') + ' ' + ' '.join(str(x) for x in a)
    print(s, flush=True)
    with open(os.path.join(WORK, 'progress_' + args.tag + '.txt'), 'a') as f: f.write(s + '\n')

class Mcp:
    def __init__(self, sock, logf):
        self.p = subprocess.Popen([MCPBIN, '--resources', os.path.join(REPO, 'resources'), '--gui', '--socket', sock, '--wait', '120'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=logf, text=True, bufsize=1)
        self.id = 0; self.lock = threading.Lock()
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
    def tool(self, name, a=None, timeout=120):
        r = self.call('tools/call', {'name': name, 'arguments': a or {}}, timeout)
        res = r.get('result', {})
        return bool(r.get('error') or res.get('isError')), ''.join(c.get('text', '') for c in res.get('content', []))
    def close(self):
        try: self.p.kill()
        except Exception: pass

PATH_KEYS = re.compile(r'path|file|dir|output|dest', re.I)
STRINGS = ['', 'a', 'Cube', 'x' * 3000, '\u00e9\u4e2d\U0001F600', '../..', '%s%n%x', '-1', 'nan', '1e309', ' ', '\n\t',
           'true', 'null', '0', 'PLA', 'Bambu PLA Basic @BBL X1C', 'Bambu Lab X1 Carbon 0.4 nozzle', 'Bambu Lab P2S 0.4 nozzle',
           '0.20mm Standard @BBL X1C', 'Textured PEI Plate', 'by_layer', 'by_object', 'layer_height', 'wall_loops',
           'prepare', 'preview', 'monitor', 'top', 'iso', 'front', 'OK', 'Cancel', 'Yes', 'No', "Don't save"]

def rand_value(schema, key='', depth=0, sb_out=''):
    if not isinstance(schema, dict): schema = {}
    if random.random() < 0.06 or depth > 4:
        return random.choice([None, -1, 0, 1, 2 ** 63, -2 ** 31, 1e308, 3.5, '', 'zz', [], {}, True])
    if 'enum' in schema and random.random() < 0.85: return random.choice(schema['enum'])
    t = schema.get('type')
    if isinstance(t, list): t = random.choice(t)
    for k in ('anyOf', 'oneOf'):
        if t is None and k in schema: return rand_value(random.choice(schema[k]), key, depth + 1, sb_out)
    if t == 'string':
        if PATH_KEYS.search(key):
            if random.random() < 0.6 and not re.search(r'out|export|save|dest', key, re.I):
                return os.path.join(CORPUS, random.choice(os.listdir(CORPUS)))
            return os.path.join(sb_out, random.choice(['p.3mf', 'o.gcode', 'x.gcode.3mf', 'y.png', 'sub/dir/z.3mf', 'q', '']))
        return random.choice(STRINGS)
    if t == 'integer': return random.choice([0, 1, 1, 2, 3, -1, 5, 100, 2 ** 31, random.randint(-5, 20)])
    if t == 'number': return random.choice([0, 0.1, 1, 2.5, -10, 45, 90, 180, 1000, 1e9, -1e-9, random.uniform(-200, 200)])
    if t == 'boolean': return random.random() < 0.5
    if t == 'array': return [rand_value(schema.get('items', {}), key, depth + 1, sb_out) for _ in range(random.choice([0, 1, 1, 2, 3, 8]))]
    if t == 'object' or 'properties' in schema:
        props = schema.get('properties', {}); req = set(schema.get('required', [])); o = {}
        for k, s in props.items():
            if k in req or random.random() < 0.5: o[k] = rand_value(s, k, depth + 1, sb_out)
        if not props and random.random() < 0.5:
            o[random.choice(['layer_height', 'sparse_infill_density', 'wall_loops', 'enable_support', 'brim_type',
                             'nozzle_temperature', 'print_sequence', 'xy_contour_compensation'])] = \
                random.choice(['0.1', '0', '-5', '100%', '1', 'nan', 'true', 'outer_only', 'by_object', '', 99999])
        return o
    return random.choice(STRINGS)

NEST_OK = {'ui_list_dialogs', 'ui_dialog_inspect', 'ui_dialog_click', 'ui_dialog_set', 'ui_close_dialog', 'ui_screenshot'}
def is_mutating(t):
    ann = t.get('annotations') or {}
    ro = bool(ann['readOnlyHint']) if 'readOnlyHint' in ann else t['name'].startswith(('get_', 'list_', 'ui_list', 'describe'))
    return not ro and t['name'] not in NEST_OK

def open_dialogs(m):
    try:
        e, t = m.tool('ui_list_dialogs', timeout=30)
        return json.loads(t).get('dialogs', []) if not e else []
    except Exception:
        return []

def mcp_loop(m, tools, alog, stop, sb_out):
    weights = [3.0 if t['name'] in ('load_model', 'open_project', 'slice', 'ui_switch_tab', 'ui_open_dialog', 'ui_dialog_click',
                                     'ui_dialog_set', 'ui_set_camera', 'undo', 'redo', 'transform_object', 'select_presets',
                                     'set_preset_settings', 'arrange_objects', 'select_plate') else
               (0.3 if t['name'] in ('ui_screenshot', 'get_events') else 1.0) for t in tools]
    while not stop.is_set():
        t = random.choices(tools, weights)[0]
        a = rand_value(t.get('inputSchema', {}), t['name'], sb_out=sb_out) if random.random() < 0.9 else {}
        if not isinstance(a, dict): a = {}
        if t['name'].startswith('ui_dialog') or t['name'] == 'ui_close_dialog':
            dl = open_dialogs(m)
            if dl and random.random() < 0.8:
                d = random.choice(dl)
                for k in ('id', 'index', 'title'):
                    if k in d: a['dialog'] = d[k]; break
        if is_mutating(t) and random.random() > args.modal_mcp_prob:
            for _ in range(6):
                if not open_dialogs(m): break
                try: m.tool('ui_close_dialog', timeout=30); stats['dialogs_closed'] += 1
                except Exception: break
                time.sleep(0.3)
        alog.write(json.dumps({'t': round(time.time(), 2), 'k': 'mcp', 'tool': t['name'], 'args': a}, default=str) + '\n'); alog.flush()
        try:
            e, txt = m.tool(t['name'], a, timeout=180)
            stats['mcp_calls'] += 1
            if e: stats['mcp_errors'] += 1
            if 'ui.blocked_by_dialog' in txt or 'ui.busy' in txt: stats['mcp_guard_refusals'] += 1
            alog.write(json.dumps({'k': 'mcp_res', 'tool': t['name'], 'err': e, 'txt': txt[:300]}) + '\n'); alog.flush()
        except TimeoutError:
            stats['mcp_errors'] += 1; alog.write(json.dumps({'k': 'mcp_timeout', 'tool': t['name']}) + '\n')
        except Exception as ex:
            alog.write(json.dumps({'k': 'mcp_dead', 'err': str(ex)}) + '\n'); return
        time.sleep(random.uniform(0.0, 1.0))

# macOS frames end in 'file.cpp:123' (no directory); Linux-style absolute paths are accepted too.
FRAME_RE = re.compile(r'#\d+ 0x[0-9a-f]+ in (.+?) (\S+?\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|inl|mm|m):\d+)(?::\d+)?\s*$', re.M)
def signatures(text):
    out = []
    starts = [x.start() for x in re.finditer(r'(?m)^(?:\S+:\d+:\d+: runtime error:|=+\n==\d+==ERROR:|==\d+==ERROR:)', text)] or ([0] if text.strip() else [])
    for blk in [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]:
        m = re.search(r'ERROR: AddressSanitizer: ([\w\-]+)', blk)
        frames = [(f[-90:], l.replace(REPO + '/', '')) for f, l in FRAME_RE.findall(blk)]
        own = [l for f, l in frames if not f.startswith('std::') and (l.startswith('src/') or '/' not in l)][:3]
        if m: out.append(('ASan ' + m.group(1), own or [l for _, l in frames[:3]], blk)); continue
        m = re.search(r'^(\S+?):(\d+):\d+: runtime error: (.*)$', blk, re.M)
        if m: out.append(('UBSan ' + m.group(3)[:120], [m.group(1).replace(REPO + '/', '') + ':' + m.group(2)], blk))
    return out

def main():
    deadline = time.time() + args.minutes * 60
    uniq = json.load(open(os.path.join(CRASHES, 'uniq.json'))) if os.path.exists(os.path.join(CRASHES, 'uniq.json')) else {}
    idx = len(glob.glob(os.path.join(OUT, 'run*')))
    while time.time() < deadline:
        idx += 1; stats['runs'] += 1
        rundir = os.path.join(OUT, 'run%03d' % idx); os.makedirs(rundir)
        sb = os.path.join(WORK, 'sb_' + args.tag); shutil.rmtree(sb, ignore_errors=True)
        for d in ('home', 'data', 'run', 'out/sub', 'tmp'): os.makedirs(os.path.join(sb, d), exist_ok=True)
        os.chmod(os.path.join(sb, 'run'), 0o700)
        fresh = not os.path.isdir(SEED) or random.random() < args.fresh_prob
        data = os.path.join(sb, 'data')
        if not fresh: shutil.rmtree(data); shutil.copytree(SEED, data, symlinks=True)
        sock = os.path.join(sb, 'run', 'mcp.sock')
        env = {k: v for k, v in os.environ.items() if not k.startswith(('DYLD_', 'SSH_'))}
        env.update({'HOME': os.path.join(sb, 'home'), 'TMPDIR': os.path.join(sb, 'tmp') + '/', 'PRINTBOT_MCP_SOCKET': sock,
                    'ASAN_OPTIONS': f'abort_on_error=1:halt_on_error=1:symbolize=1:log_path={rundir}/asan:detect_leaks=0:'
                                    f'allocator_may_return_null=1:detect_stack_use_after_return=0:print_summary=1:'
                                    # The static deps are not instrumented and share libc++ vector instantiations with the
                                    # app (ODR), which leaves libc++'s container annotations stale: false overflow reports.
                                    f'detect_container_overflow=0',
                    'UBSAN_OPTIONS': f'print_stacktrace=1:halt_on_error=0:log_path={rundir}/ubsan:suppressions={cfg.SUPP}'})
        cmd = [APP, '--datadir', data]
        if not args.no_sandbox: cmd = ['sandbox-exec', '-p', SANDBOX] + cmd
        applog = open(os.path.join(rundir, 'app.log'), 'w')
        p = subprocess.Popen(cmd, env=env, stdout=applog, stderr=subprocess.STDOUT, cwd=os.path.join(sb, 'out'), start_new_session=True)
        log(f'run {idx} pid {p.pid} fresh={fresh}')
        alog = open(os.path.join(rundir, 'actions.jsonl'), 'w')
        m = Mcp(sock, open(os.path.join(rundir, 'mcp.log'), 'w'))
        stop = threading.Event(); hang = False
        try:
            m.call('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'fuzz_mac', 'version': '1'}}, timeout=240)
            m.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n'); m.p.stdin.flush()
            tools = [t for t in m.call('tools/list', {}, timeout=60)['result']['tools'] if cfg.tool_allowed(t['name'])]
            th = threading.Thread(target=mcp_loop, args=(m, tools, alog, stop, os.path.join(sb, 'out')), daemon=True); th.start()
            budget = random.uniform(300, 600)
            t0 = time.time()
            while p.poll() is None and time.time() - t0 < budget and time.time() < deadline and th.is_alive():
                time.sleep(2)
            if p.poll() is None and not th.is_alive():
                # MCP died while the app is still up: check the app still answers, else count a hang
                try: Mcp(sock, subprocess.DEVNULL).call('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                                                      'clientInfo': {'name': 'probe', 'version': '1'}}, timeout=60)
                except Exception: hang = True
        except Exception as e:
            log('mcp setup failed', repr(e))
        stop.set()
        if p.poll() is None:
            if not os.path.isdir(SEED) and os.listdir(data):
                shutil.copytree(data, SEED, symlinks=True, ignore=shutil.ignore_patterns('log', 'cache', '*.lock'))
            os.killpg(p.pid, signal.SIGTERM)
            try: p.wait(30)
            except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL); p.wait()
            rc = 'killed'
        else:
            rc = p.returncode
        m.close(); alog.close(); time.sleep(1)
        crashed = rc not in (0, 'killed')
        stats['crashes'] += int(crashed); stats['clean_exits'] += int(rc == 0); stats['hangs'] += int(hang)
        for f in sorted(glob.glob(os.path.join(rundir, 'asan.*')) + glob.glob(os.path.join(rundir, 'ubsan.*'))):
            for kind, where, blk in signatures(open(f, errors='replace').read()):
                key = kind + '|' + '|'.join(where)
                if key not in uniq:
                    cd = os.path.join(CRASHES, 'c%03d' % (len(uniq) + 1)); os.makedirs(cd)
                    open(os.path.join(cd, 'report.txt'), 'w').write(blk)
                    shutil.copy(os.path.join(rundir, 'actions.jsonl'), os.path.join(cd, 'actions_before.jsonl'))
                    uniq[key] = {'dir': cd, 'kind': kind, 'where': where, 'count': 0, 'runs': []}
                    log('NEW finding', kind, where)
                uniq[key]['count'] += 1; uniq[key]['runs'].append(idx)
        if crashed and not glob.glob(os.path.join(rundir, 'asan.*')) + glob.glob(os.path.join(rundir, 'ubsan.*')):
            log(f'run {idx}: exit rc={rc} without a sanitizer report (see {rundir}/app.log)')
        log(f'run {idx} ended rc={rc} mcp={stats["mcp_calls"]}')
        json.dump(uniq, open(os.path.join(CRASHES, 'uniq.json'), 'w'), indent=1)
        json.dump(stats, open(os.path.join(WORK, 'stats_' + args.tag + '.json'), 'w'), indent=1)
    log('DONE', stats)

if __name__ == '__main__':
    main()
