#!/usr/bin/env python3
"""Replay every crash/UB/leak repro found by the fuzz campaigns against the ASan build, through the live MCP
endpoint, and record per-step results. Each step prints one JSON line; 'alive': false means the app died.

  repro.py            full repro list (sanitizer reports land in <work>/repro[_leaks]/)
  repro.py --leaks    same steps with detect_leaks=1 and a LeakSanitizer check before exit
  repro.py --spiral   only the spiral-vase repro;  --range: only the range-check + context-menu repros

Uses the profile seeded by fuzz.py (<work>/seed_home) if present, else an empty profile.
Paths/env overrides: see fuzzcfg.py (--app/--mcp/--work/--display flags too)."""
import os, sys, json, time, subprocess, shutil, threading, glob, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzcfg as cfg
_ap = argparse.ArgumentParser(description='Replay the known fuzz findings against the sanitizer build.')
_ap.add_argument('--leaks', action='store_true'); _ap.add_argument('--section', default=None, help='run only one focused section: slice_idle | validate_image'); _ap.add_argument('--spiral', action='store_true'); _ap.add_argument('--range', action='store_true')
_ap.add_argument('--app', default=cfg.APP); _ap.add_argument('--mcp', default=cfg.MCPBIN)
_ap.add_argument('--work', default=cfg.WORK); _ap.add_argument('--corpus', default=None); _ap.add_argument('--display', default=cfg.DISP)
_a = _ap.parse_args()
LEAKS, ONLY_SPIRAL, ONLY_RANGE = _a.leaks, _a.spiral, _a.range
FZ = os.path.abspath(_a.work); REPO = cfg.REPO; cfg.WORK = FZ; cfg.DISP = DISP = _a.display
APP, MCPBIN = os.path.abspath(_a.app), os.path.abspath(_a.mcp)
C = os.path.abspath(_a.corpus or os.environ.get('PRINTBOT_FUZZ_CORPUS') or os.path.join(FZ, 'corpus')); K = C
for _p in (APP, MCPBIN, C):
    if not os.path.exists(_p): sys.exit(f'{_p} does not exist (build it / run gen_corpus.py, or pass the path)')
OUT = os.path.join(FZ, 'repro' + ('_leaks' if LEAKS else '') + ('_spiral' if ONLY_SPIRAL else '') + ('_range' if ONLY_RANGE else '')); SB = os.path.join(FZ, 'sb_repro')
shutil.rmtree(SB, ignore_errors=True); os.makedirs(OUT, exist_ok=True)
if os.path.isdir(os.path.join(FZ, 'seed_home')):
    shutil.copytree(os.path.join(FZ, 'seed_home'), os.path.join(SB, 'home'), symlinks=True)
else:
    print('note: no seed profile (run fuzz.py once to create <work>/seed_home); starting from an empty profile', flush=True)
    os.makedirs(os.path.join(SB, 'home'))
for d in ['run', 'out', 'tmp']: os.makedirs(os.path.join(SB, d), exist_ok=True)
if True:  # sandbox copy only: re-enable the one-shot 'Parameter recommendation' ImageMessageDialog
    _cf = os.path.join(SB, 'home/.config/PrintBot/BambuStudio.conf')
    if os.path.exists(_cf):
        _txt = open(_cf).read().replace('"show_wrapping_detect_dialog": false', '"show_wrapping_detect_dialog": true')
        open(_cf, 'w').write(_txt)
os.chmod(os.path.join(SB, 'run'), 0o700)
H = os.path.join(SB, 'home'); sock = os.path.join(SB, 'run/mcp.sock')
env = {k: v for k, v in os.environ.items() if k not in ('WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'LD_LIBRARY_PATH', 'PYTHONHOME', 'PYTHONPATH')}
env.update(DISPLAY=DISP, GDK_BACKEND='x11', HOME=H, XDG_CONFIG_HOME=H + '/.config', XDG_CACHE_HOME=H + '/.cache',
           XDG_DATA_HOME=H + '/.local/share', XDG_RUNTIME_DIR=SB + '/run', TMPDIR=SB + '/tmp', PRINTBOT_MCP_SOCKET=sock,
           DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent', PATH=cfg.stub_bin_dir() + ':' + os.environ['PATH'], FUZZ_SB=SB,
           SSL_CERT_FILE=cfg.SSL_CERT_FILE, ASAN_SYMBOLIZER_PATH=cfg.symbolizer(),
           ASAN_OPTIONS=f'detect_leaks={int(LEAKS)}:abort_on_error=1:halt_on_error=1:log_path={OUT}/asan',
           UBSAN_OPTIONS=f'print_stacktrace=1:log_path={OUT}/ubsan:suppressions={cfg.SUPP}')
# start clean: a leftover trigger file would make the shim end the new run at once, and old reports would mix in
for _f in os.listdir(OUT):
    if _f in ('do_leakcheck', 'results.json', 'app.log') or _f.startswith(('asan.', 'ubsan.')): os.remove(os.path.join(OUT, _f))
if LEAKS:
    env['LD_PRELOAD'] = cfg.lsan_shim(); env['FUZZ_LEAKCHECK_FILE'] = OUT + '/do_leakcheck'
if subprocess.run(['unshare', '-cn', 'true'], capture_output=True).returncode != 0:
    sys.exit('unshare -cn is not available; refusing to run the app with network access')
if not cfg.wait_display_free():
    sys.exit(f'display {DISP} is in use; pick another with --display')
xv = subprocess.Popen(['Xvfb', DISP, '-screen', '0', '1920x1080x24', '-nolisten', 'tcp'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
app = subprocess.Popen(['unshare', '-cn', APP], env=env, stdout=open(OUT + '/app.log', 'w'), stderr=subprocess.STDOUT, cwd=SB + '/out', start_new_session=True)

class Mcp:
    def __init__(s):
        s.p = subprocess.Popen([MCPBIN, '--resources', REPO + '/resources', '--gui', '--socket', sock, '--wait', '180'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1); s.i = 0
        s.req('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'repro', 'version': '1'}})
        s.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n'); s.p.stdin.flush()
    def req(s, m, params):
        s.i += 1; s.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': s.i, 'method': m, 'params': params}) + '\n'); s.p.stdin.flush()
        while True:
            l = s.p.stdout.readline()
            if not l: raise EOFError('mcp closed (app died?)')
            j = json.loads(l)
            if j.get('id') == s.i: return j
    def call(s, name, args=None):
        r = s.req('tools/call', {'name': name, 'arguments': args or {}})
        res = r.get('result', {}); txt = ''.join(c.get('text', '') for c in res.get('content', []))
        return bool(res.get('isError') or r.get('error')), txt

results = []
def spiral():
    global drain_after
    step('9 spiral vase slice', 'clear_project', {})
    step('9 spiral vase slice', 'load_model', {'path': C + '/ok_cone.stl'})
    drain_after = False
    step('9 spiral vase slice', 'set_preset_settings', {'type': 'process', 'values': {'spiral_mode': '1'}})
    for _ in range(4):  # answer the "adjust settings for spiral mode?" prompt with Yes/OK
        e, t = M.call('ui_list_dialogs', {})
        if '"title"' not in t: break
        e2, t2 = M.call('ui_dialog_click', {'button': 'Yes'})
        if e2: M.call('ui_dialog_click', {'button': 'OK'})
        time.sleep(1)
    drain_after = True
    step('9 spiral vase check', 'get_preset_settings', {'type': 'process', 'keys': ['spiral_mode']})
    step('9 spiral vase slice', 'slice', {})
    step('9 non-spiral slice', 'set_preset_settings', {'type': 'process', 'values': {'spiral_mode': '0'}})
    step('9 non-spiral slice', 'slice', {})

def step(label, name, args=None, m=None):
    m = m or M
    global drain_after
    t = time.time()
    try: err, txt = m.call(name, args)
    except Exception as e: err, txt = True, 'EXC ' + repr(e)
    alive = app.poll() is None
    results.append({'repro': label, 'tool': name, 'err': err, 'alive': alive, 'secs': round(time.time() - t, 1), 'msg': txt[:220]})
    print(json.dumps(results[-1]), flush=True)
    if not alive: raise SystemExit('APP DIED at ' + label)
    if drain_after: drain(m)
    return err, txt

def drain(m):
    # close dialogs the step left behind (load warnings / error boxes) so the next step starts clean
    for _ in range(8):
        e, t = m.call('ui_list_dialogs', {})
        try: dl = json.loads(t).get('dialogs', [])
        except Exception: dl = []
        if not dl: return
        results.append({'repro': '(drain)', 'closed': [d.get('title') for d in dl]})
        print(json.dumps(results[-1]), flush=True)
        m.call('ui_close_dialog', {}); time.sleep(1.5)
drain_after = True

def clean_presets(m=None):
    # no pending dialogs and no unsaved preset edits, so preset switches don't stop at 'Save or Discard'
    drain(m or M)
    for t in ('printer', 'process', 'filament'):
        step('(clean) discard ' + t, 'discard_preset_changes', {'type': t}, m=m)

def sec_absurd():
    # Print::validate must refuse an absurd length setting before slicing. xy_contour/hole compensation and
    # elephant foot are already reset by the app above small limits (info dialog), so use settings without such a guard.
    clean_presets()
    step('13 absurd mm setting', 'clear_project', {'discard_unsaved': True})
    step('13 absurd mm setting', 'load_model', {'path': C + '/ok_cone.stl'})
    e, t = M.call('list_objects', {}); oid = json.loads(t)['objects'][0]['id']
    for label, tool, args in [
            ('process top_shell_thickness 999999', 'set_preset_settings', {'type': 'process', 'values': {'top_shell_thickness': 999999}}),
            ('object xy_contour_compensation 999999', 'set_object_settings', {'id': oid, 'values': {'xy_contour_compensation': 999999}})]:
        step('13 absurd: ' + label, tool, args)
        err, txt = step('13 absurd: slice refused (' + label + ')', 'slice', {'timeout_s': 60})
        results.append({'repro': '13 CHECK validate refused ' + label, 'pass': bool(err and 'far outside' in txt), 'msg': txt[:200]})
        print(json.dumps(results[-1]), flush=True)
        if tool == 'set_preset_settings': step('13 absurd: reset', 'discard_preset_changes', {'type': 'process'})
        else: step('13 absurd: reset', 'set_object_settings', {'id': oid, 'values': {'xy_contour_compensation': 0}})
    step('13 absurd mm setting: slice ok again', 'slice', {'timeout_s': 120})

def sec_image(m2):
    # Switching to a wrapping-detection printer shows ImageMessageDialog ('Parameter recommendation') once.
    # It is queued by the preset switch, so it may open only during the next state-changing call.
    clean_presets()
    global drain_after
    drain_after = False
    step('15 image dialog: A2L preset', 'select_presets', {'printer': 'Bambu Lab A2L 0.4 nozzle'})
    seen = []
    for _ in range(20):
        M.call('get_status', {})          # any call wakes the GUI event loop
        time.sleep(1)
        e, t = m2.call('ui_list_dialogs', {})
        try: seen = [d.get('title') for d in json.loads(t).get('dialogs', [])]
        except Exception: seen = []
        if seen: break
    # The dialog is queued by the preset switch and can open during the next state-changing call instead.
    err, txt = step('15 image dialog: back to P2S', 'select_presets', {'printer': 'Bambu Lab P2S 0.4 nozzle'})
    # it shows once per profile, so in the full repro an earlier preset change may already have opened it
    shown = any('Parameter recommendation' in (x or '') for x in seen) or (err and 'Parameter recommendation' in txt) or \
        any('Parameter recommendation' in str(r.get('closed', '')) for r in results)
    results.append({'repro': '15 CHECK image dialog shown', 'pass': bool(shown), 'dialogs': seen, 'msg': txt[:160]})
    print(json.dumps(results[-1]), flush=True)
    drain_after = True
    drain(m2); drain(M)
    if err: step('15 image dialog: back to P2S (after closing it)', 'select_presets', {'printer': 'Bambu Lab P2S 0.4 nozzle'})

try:
    M = Mcp()
    if _a.section == 'validate_image':
        sec_absurd(); sec_image(Mcp()); raise SystemExit(0)
    if _a.section == 'slice_idle':
        # A slice after a failed G-code export (bad placeholder in the end G-code) used to sit idle until timeout.
        step('16 slice idle', 'clear_project', {})
        step('16 slice idle', 'load_model', {'path': C + '/ok_cone.stl'})
        step('16 slice idle: bad end gcode', 'set_preset_settings', {'type': 'printer', 'values': {'machine_end_gcode': '; {"ab" + undefined_variable_xyz}\n'}})
        step('16 slice idle: slice 1 (export error expected)', 'slice', {'timeout_s': 90})
        step('16 slice idle: slice 2 (must not idle)', 'slice', {'timeout_s': 60})
        step('16 slice idle: status', 'get_slice_status', {})
        step('16 slice idle: fix end gcode', 'set_preset_settings', {'type': 'printer', 'values': {'machine_end_gcode': '; end\n'}})
        step('16 slice idle: slice 3 (ok expected)', 'slice', {'timeout_s': 90})
        raise SystemExit(0)
    if ONLY_SPIRAL:
        spiral(); raise SystemExit(0)
    if ONLY_RANGE:
        step('11 range', 'load_model', {'path': C + '/ok_cone.stl'})
        step('11 range: wall_loops 99999 refused', 'set_preset_settings', {'type': 'process', 'values': {'wall_loops': 99999}})
        step('11 range: layer_height -1 refused', 'set_preset_settings', {'type': 'process', 'values': {'layer_height': -1}})
        e, t = M.call('list_objects', {})
        oid = json.loads(t)['objects'][0]['id']
        step('11 range: object wall_loops 99999 refused', 'set_object_settings', {'id': oid, 'values': {'wall_loops': 99999}})
        step('11 range: wall_loops 50 ok', 'set_preset_settings', {'type': 'process', 'values': {'wall_loops': 50}})
        # 12: context menu open -> state-changing MCP calls refused (was a UAF: undo under a plate right-click menu)
        step('12 popup', 'ui_switch_tab', {'tab': 'prepare'})
        step('12 popup', 'select_objects', {'ids': [oid]})
        xenv = cfg.x_env(); XDOTOOL = cfg.xdotool()
        ws = subprocess.run([XDOTOOL, 'search', '--onlyvisible', '--pid', str(app.pid)], env=xenv, capture_output=True, text=True).stdout.split()
        best = None
        for w in ws:
            g = subprocess.run([XDOTOOL, 'getwindowgeometry', '--shell', w], env=xenv, capture_output=True, text=True).stdout
            d = dict(l.split('=', 1) for l in g.splitlines() if '=' in l)
            if best is None or int(d['WIDTH']) * int(d['HEIGHT']) > best[1]: best = (w, int(d['WIDTH']) * int(d['HEIGHT']), int(d['WIDTH']), int(d['HEIGHT']))
        w, _, W, Hh = best
        n0 = len(ws); opened = False
        for fx, fy in [(0.35, 0.5), (0.33, 0.55), (0.38, 0.45), (0.3, 0.5), (0.4, 0.6), (0.9, 0.3)]:
            subprocess.run([XDOTOOL, 'mousemove', '--window', w, str(int(W * fx)), str(int(Hh * fy)), 'click', '3'], env=xenv)
            time.sleep(1.5)
            n1 = len(subprocess.run([XDOTOOL, 'search', '--onlyvisible', '--pid', str(app.pid)], env=xenv, capture_output=True, text=True).stdout.split())
            if n1 > n0:
                opened = True; break
            subprocess.run([XDOTOOL, 'key', 'Escape'], env=xenv); time.sleep(0.5)
        results.append({'repro': '12 popup: context menu opened', 'opened': opened}); print(json.dumps(results[-1]), flush=True)
        drain_after = False
        step('12 popup: undo refused while context menu open', 'undo', {})
        step('12 popup: delete refused while context menu open', 'clear_project', {})
        subprocess.run([XDOTOOL, 'key', 'Escape'], env=xenv); time.sleep(1)
        drain_after = True
        step('12 popup: undo works after menu closed', 'undo', {})
        step('11 range: slice', 'slice', {})
        raise SystemExit(0)
    step('1 stl header overread', 'load_model', {'path': K + '/ok_Bambu_Cube.stl'})
    step('1 stl header overread', 'load_model', {'path': K + '/ok_20mmbox-CR.stl', 'append': True})
    step('1 stl header overread', 'load_model', {'path': K + '/bad_random.stl', 'append': True})
    step('4 select_plate 0', 'select_plate', {'plate': 0})
    step('4 select_plate 1', 'select_plate', {'plate': 1})
    step('6 nan 3mf rejected', 'load_model', {'path': C + '/bad3_nanverts.3mf'})
    step('6 nan stl rejected', 'load_model', {'path': C + '/bad_nan.stl'})
    step('6 nan obj rejected', 'load_model', {'path': C + '/bad_obj_nan.obj'})
    step('6 empty plate slice', 'clear_project', {})
    step('6 empty plate slice', 'slice', {})
    step('2 floating concentric slice', 'load_model', {'paths': [C + '/ok_torus.stl', C + '/ok_3DBenchy.stl', C + '/ok_cone.stl']})
    step('2 floating concentric slice', 'arrange_objects', {})
    step('2 floating concentric slice', 'slice', {})
    step('3 preset change', 'select_presets', {'printer': 'Bambu Lab X1 Carbon 0.4 nozzle'})
    step('3 preset change', 'select_presets', {'printer': 'Bambu Lab P2S 0.4 nozzle'})
    step('3 preset change + slice', 'slice', {})
    # 5/7: re-entrancy: a broken project opens an error modal; a second client must be refused, not nested.
    M2 = Mcp()
    drain_after = False
    res2 = {}
    def bg():
        try: res2['r'] = M.call('open_project', {'path': C + '/badbbl_flip1.3mf', 'discard_unsaved': True})
        except Exception as e: res2['r'] = (True, repr(e))
    th = threading.Thread(target=bg, daemon=True); th.start(); time.sleep(3)
    step('7 reentrancy: mutate while modal', 'load_model', {'path': C + '/ok_cone.stl'}, m=M2)
    step('7 reentrancy: select_plate while modal', 'select_plate', {'plate': 1}, m=M2)
    step('7 reentrancy: dialogs still listable', 'ui_list_dialogs', {}, m=M2)
    err, txt = step('7 reentrancy: answer dialog', 'ui_list_dialogs', {}, m=M2)
    for _ in range(4):
        e, t = M2.call('ui_list_dialogs', {})
        if '"modal": true' not in t and '"modal":true' not in t: break
        step('7 reentrancy: close dialog', 'ui_close_dialog', {}, m=M2); time.sleep(1)
    th.join(60); print('open_project result', res2.get('r'), flush=True)
    drain_after = True; drain(M)
    step('5 broken 3mf then load', 'load_model', {'path': C + '/badbbl_flip3.3mf'})
    step('5 broken 3mf then load', 'load_model', {'paths': [C + '/bad_count_short.stl', C + '/badbbl_flip5.3mf']})
    # spiral vase (GCode.cpp:3665 null-reference) on and off
    spiral()
    # leak paths: malformed 3MF XML (miniz callback throw), Prusa-check parser, placeholder string expressions
    for f in ['bad3_truncxml.3mf', 'badbbl_flip0.3mf', 'badbbl_flip2.3mf', 'badbbl_flip4.3mf', 'bad3_badidx.3mf', 'bad3_flip2.3mf']:
        step('10 malformed 3mf', 'load_model', {'path': C + '/' + f})
    step('10 placeholder strings', 'clear_project', {})
    step('10 placeholder strings', 'load_model', {'path': C + '/ok_cone.stl'})
    step('10 placeholder strings', 'set_preset_settings', {'type': 'printer', 'values': {'machine_end_gcode':
         '; {"ab" + "cd"} {if "x" == "x"}eq{else}ne{endif} {("a" + "b") + "c"}\n'}})
    step('10 placeholder strings', 'slice', {})
    step('10 placeholder error', 'set_preset_settings', {'type': 'printer', 'values': {'machine_end_gcode':
         '; {"ab" + undefined_variable_xyz} {"q" + }\n'}})
    step('10 placeholder error', 'slice', {})
    # 13: huge object scale -> must be refused at validation, not overflow the clipper in discover_vertical_shells
    step('13 huge scale', 'clear_project', {})
    step('13 huge scale', 'load_model', {'path': C + '/ok_cone.stl'})
    e, t = M.call('list_objects', {}); oid = json.loads(t)['objects'][0]['id']
    step('13 huge scale: scale 1e6', 'transform_object', {'id': oid, 'scale': 1e6})
    step('13 huge scale: slice refused', 'slice', {'timeout_s': 60})
    step('13 huge position', 'transform_object', {'id': oid, 'scale': 1, 'position': [1e7, 1e7]})
    step('13 huge position: slice', 'slice', {'timeout_s': 60})
    sec_absurd()
    e, t = M.call('list_objects', {}); oid = json.loads(t)['objects'][0]['id']
    # 14: undo/redo cycles (UndoRedo.cpp:881 leaked ModelInstance per undo)
    for i in range(3):
        step('14 undo leak: move', 'transform_object', {'id': oid, 'position': [100 + 10 * i, 100]})
        step('14 undo leak: undo', 'undo', {})
        step('14 undo leak: redo', 'redo', {})
    step('14 undo leak: undo', 'undo', {})
    # 15: multi-object 3MF export (bbs_3mf heap-archive leak) and ImageMessageDialog (wrapping-detect printer)
    step('15 3mf export', 'load_model', {'paths': [C + '/ok_torus.stl', C + '/ok_cone.stl']})
    step('15 3mf export', 'arrange_objects', {})
    step('15 3mf export', 'save_project', {'path': SB + '/out/leak_export.3mf'})
    sec_image(M2)
    step('final status', 'get_status', {})
finally:
    json.dump(results, open(OUT + '/results.json', 'w'), indent=1)
    if LEAKS and app.poll() is None:
        open(OUT + '/do_leakcheck', 'w').close()
        try: app.wait(240)
        except Exception: pass
    try: os.killpg(app.pid, 15); app.wait(20)
    except Exception: 
        try: os.killpg(app.pid, 9)
        except Exception: pass
    xv.terminate()
    try: xv.wait(15)
    except Exception: pass
    print('reports:', glob.glob(OUT + '/asan*') + glob.glob(OUT + '/ubsan*'))
