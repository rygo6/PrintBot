#!/usr/bin/env python3
"""Generate the fuzz input corpus: a few valid models copied from the repo (resources/, tests/data/) plus
deterministic malformed STL/OBJ/STEP/3MF variants (truncated, NaN/inf, huge counts, bit-flipped XML, ...).
Output: $PRINTBOT_FUZZ_CORPUS, else <work>/corpus (default <repo>/temp/fuzz/corpus).

Usage: gen_corpus.py [outdir] [--bbl-project PROJECT.3mf]
  --bbl-project  a Bambu-format project saved by PrintBot itself (e.g. slice a model and "Save project as",
                 or take any .3mf the fuzzer saved under <work>/sb_*/out/). It is copied as ok_bambu_project.3mf and
                 mutated into badbbl_*.3mf (missing plates/settings, garbage G-code, bit flips). repro.py uses
                 several badbbl_* files; without them those steps just report a load error."""
import struct, os, sys, zipfile, random, shutil, glob, io
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzcfg
repo = fuzzcfg.REPO
_args = sys.argv[1:]
BBL = None
if '--bbl-project' in _args:
    i = _args.index('--bbl-project'); BBL = os.path.abspath(_args[i + 1]); del _args[i:i + 2]
C = os.path.abspath(_args[0]) if _args else fuzzcfg.CORPUS
os.makedirs(C, exist_ok=True)
random.seed(1234)
good = []
for p in ['resources/model/3DBenchy.stl','resources/model/Bambu_Cube.stl','resources/model/torus.stl','resources/model/cone.stl',
          'tests/data/20mm_cube.obj','tests/data/ipadstand.obj','tests/data/test_stl/ASCII/20mmbox-CR.stl',
          'tests/data/test_3mf/Geräte/Büchse.3mf']:
    src = os.path.join(repo, p); dst = os.path.join(C, 'ok_' + os.path.basename(p).replace('ä','a').replace('ü','u'))
    shutil.copy(src, dst); good.append(dst)
def tri_bin(tris, n=None):
    b = b'\0'*80 + struct.pack('<I', len(tris) if n is None else n)
    for t in tris:
        b += struct.pack('<12fH', 0,0,1, *[c for v in t for c in v], 0)
    return b
cube = [((0,0,0),(10,0,0),(0,10,0)), ((10,0,0),(10,10,0),(0,10,0)), ((0,0,0),(0,0,10),(10,0,0))]
w = lambda n, d: open(os.path.join(C, n), 'wb').write(d)
w('bad_empty.stl', b'')
w('bad_header_only.stl', b'\0'*80)
w('bad_count_huge.stl', tri_bin(cube, n=0xFFFFFFF0))
w('bad_count_short.stl', tri_bin(cube, n=1000))
w('bad_truncated.stl', tri_bin(cube)[:-17])
nan = float('nan'); inf = float('inf')
w('bad_nan.stl', tri_bin([((nan,0,0),(1,nan,0),(0,1,inf))]+cube))
w('bad_huge_coords.stl', tri_bin([((1e30,0,0),(-1e30,1e30,0),(0,-1e30,1e30))]))
w('bad_degenerate.stl', tri_bin([((0,0,0),(0,0,0),(0,0,0))]*50))
w('bad_tiny.stl', tri_bin([((0,0,0),(1e-9,0,0),(0,1e-9,0))]))
w('bad_ascii.stl', b'solid x\nfacet normal 0 0 1\nouter loop\nvertex 1 2\nvertex a b c\nendloop\nendfacet\nfacet\nendsolid\n')
w('bad_ascii_unterminated.stl', b'solid x\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\n')
w('bad_random.stl', os.urandom(5000))
w('bad_obj_idx.obj', b'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 99\nf -5 2 3\nf 0 1 2\nf 1/2/3 2//4 3\n')
w('bad_obj_nan.obj', b'v nan 0 0\nv 1 inf 0\nv 0 1 -inf\nf 1 2 3\n')
w('bad_obj_noverts.obj', b'f 1 2 3\nf 4 5 6 7 8 9\n')
w('bad_obj_bigpoly.obj', b''.join(b'v %f %f 0\n' % (random.random()*50, random.random()*50) for _ in range(500)) + b'f ' + b' '.join(str(i+1).encode() for i in range(500)) + b'\n')
w('bad_random.3mf', os.urandom(4000))
w('bad_empty.3mf', b'')
w('bad_step.step', b'ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=GARBAGE((#2,#3));\nENDSEC;\nEND-ISO-10303-21;\n')
w('bad_random.step', os.urandom(3000))
# mutated 3mfs from the good one
src3 = good[-1]
zin = zipfile.ZipFile(src3)
names = zin.namelist()
def mk3(name, mut):
    out = io.BytesIO(); zo = zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED)
    for n in names:
        d = zin.read(n); d2 = mut(n, d)
        if d2 is not None: zo.writestr(n, d2)
    zo.close(); w(name, out.getvalue())
model = [n for n in names if n.endswith('.model')]
mk3('bad3_nomodel.3mf', lambda n,d: None if n.endswith('.model') else d)
mk3('bad3_truncxml.3mf', lambda n,d: d[:len(d)//2] if n.endswith('.model') else d)
mk3('bad3_badidx.3mf', lambda n,d: d.replace(b'v1="', b'v1="9999999').replace(b'v2="', b'v2="-') if n.endswith('.model') else d)
mk3('bad3_nanverts.3mf', lambda n,d: d.replace(b'x="', b'x="nan', 50).replace(b'y="', b'y="1e40', 50) if n.endswith('.model') else d)
mk3('bad3_badconfig.3mf', lambda n,d: (os.urandom(200) if n.endswith('.config') or n.endswith('.json') else d))
mk3('bad3_badrels.3mf', lambda n,d: b'<garbage' if n.endswith('.rels') else d)
for i in range(6):
    def flip(n, d, i=i):
        if not n.endswith(('.model','.config')): return d
        b = bytearray(d)
        for _ in range(max(1, len(b)//500)):
            b[random.randrange(len(b))] = random.choice(b'<>"/=0123456789-.e \0')
        return bytes(b)
    mk3(f'bad3_flip{i}.3mf', flip)
raw = open(src3,'rb').read()
w('bad3_trunczip.3mf', raw[:len(raw)//2])
# Bambu-format project mutations (needs a project written by the app itself, see --bbl-project)
if BBL:
    shutil.copy(BBL, os.path.join(C, 'ok_bambu_project.3mf'))
    zb = zipfile.ZipFile(BBL); bnames = zb.namelist()
    def mkb(name, mut, extra=None):
        out = io.BytesIO(); zo = zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED)
        for n in bnames:
            d2 = mut(n, zb.read(n))
            if d2 is not None: zo.writestr(n, d2)
        for n, d in (extra or {}).items(): zo.writestr(n, d)
        zo.close(); w(name, out.getvalue())
    mkb('badbbl_noplates.3mf', lambda n, d: None if n.startswith('Metadata/plate_') else d)
    mkb('badbbl_nosettings.3mf', lambda n, d: None if n.endswith('project_settings.config') else d)
    mkb('badbbl_settings_empty.3mf', lambda n, d: b'{}' if n.endswith('project_settings.config') else d)
    has_gcode = any(n.endswith('.gcode') for n in bnames)
    mkb('badbbl_gcode_garbage.3mf', lambda n, d: os.urandom(4096) if n.endswith('.gcode') else d,
        None if has_gcode else {'Metadata/plate_1.gcode': os.urandom(4096)})
    for i in range(8):
        def bflip(n, d, i=i):
            if not n.endswith(('.model', '.config', '.json', '.rels', '.xml')): return d
            b = bytearray(d)
            for _ in range(max(1, len(b) // 400)):
                b[random.randrange(len(b))] = random.choice(b'<>"/=0123456789-.e \0')
            return bytes(b)
        mkb(f'badbbl_flip{i}.3mf', bflip)
else:
    print('note: no --bbl-project given; badbbl_*.3mf (Bambu project mutations) not generated')
print(len(os.listdir(C)), 'files in', C)
