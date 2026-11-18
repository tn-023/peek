# Prueft die Kacheltextur: erzeugt PNG-Sequenzen in verschiedenen Formaten, laesst peek die Textur bauen
# und vergleicht jede Kachel Wert fuer Wert mit dem Einzelbild.  Aufruf: python atlastest.py <peek.exe> <arbeitsordner>
import os, random, subprocess, sys
import numpy as np
import png

exe, root = sys.argv[1], sys.argv[2]
rng = random.Random(42)
os.makedirs(root, exist_ok=True)
failed = 0


def write(path, w, h, planes, bitdepth, rows, **kw):
    with open(path, 'wb') as f:
        png.Writer(w, h, bitdepth=bitdepth, **kw).write(f, rows)


def rand_rows(w, h, planes, maxv):
    return [[rng.randint(0, maxv) for _ in range(w * planes)] for _ in range(h)]


def read_direct(path):
    w, h, rows, info = png.Reader(filename=path).asDirect()
    planes = info['planes']
    a = np.array([list(r) for r in rows], dtype=np.uint32).reshape(h, w, planes)
    return a, info['bitdepth']


def make(name, n, w, h, kind, names=None):
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    for i in range(n):
        fn = os.path.join(d, (names[i] if names else f'frame_{i:04d}') + '.png')
        if kind == 'rgba8':
            rows = rand_rows(w, h, 4, 255)
            for r in rows:   # halbtransparente und voll transparente Pixel mit Farbwerten
                for x in range(0, len(r), 4):
                    r[x + 3] = rng.choice([0, 1, 77, 128, 254, 255])
            write(fn, w, h, 4, 8, rows, alpha=True, greyscale=False)
        elif kind == 'rgb8':
            write(fn, w, h, 3, 8, rand_rows(w, h, 3, 255), greyscale=False)
        elif kind == 'rgba16':
            write(fn, w, h, 4, 16, rand_rows(w, h, 4, 65535), alpha=True, greyscale=False)
        elif kind == 'rgb16':
            write(fn, w, h, 3, 16, rand_rows(w, h, 3, 65535), greyscale=False)
        elif kind == 'gray16':
            write(fn, w, h, 1, 16, rand_rows(w, h, 1, 65535), greyscale=True)
        elif kind == 'gray8':
            write(fn, w, h, 1, 8, rand_rows(w, h, 1, 255), greyscale=True)
        elif kind == 'gray4':
            write(fn, w, h, 1, 4, rand_rows(w, h, 1, 15), greyscale=True)
        elif kind == 'pal':
            pal = [(rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255), rng.choice([0, 90, 255])) for _ in range(16)]
            write(fn, w, h, 1, 8, rand_rows(w, h, 1, 15), palette=pal)
    return d


def check(name, d, cols=0, expect_fail=False):
    global failed
    out = os.path.join(root, name + '.png')
    args = [exe, '--kacheltextur-test', d, out] + ([str(cols)] if cols else [])
    code = subprocess.run(args).returncode
    if expect_fail:
        ok = code != 0 and not os.path.exists(out)
        err = open(out + '.fehler.txt', encoding='utf-8').read() if os.path.exists(out + '.fehler.txt') else ''
        print(f'{name}: erwarteter Fehler {"ok" if ok else "FEHLT"} ({code}) {err}')
        failed += 0 if ok else 1
        return
    if code != 0:
        print(f'{name}: FEHLER Rueckgabe {code}')
        failed += 1
        return
    files = sorted(os.listdir(d), key=lambda s: [int(t) if t.isdigit() else t for t in __import__('re').split(r'(\d+)', s)])
    tex, tbits = read_direct(out)
    th, tw = None, None
    bad = 0
    for i, f in enumerate(files):
        src, sbits = read_direct(os.path.join(d, f))
        h, w, p = src.shape
        if sbits != tbits:   # Graustufen < 8 Bit -> 8 Bit (WIC skaliert exakt)
            src = src * ((2 ** tbits - 1) // (2 ** sbits - 1))
        if p == 3 and tex.shape[2] == 4:   # Palette ohne Transparenz -> RGBA
            src = np.concatenate([src, np.full((h, w, 1), 2 ** tbits - 1, dtype=np.uint32)], axis=2)
        if th is None:
            th, tw = h, w
            ncols = tex.shape[1] // w
        c, r = i % ncols, i // ncols
        tile = tex[r * h:(r + 1) * h, c * w:(c + 1) * w]
        if tile.shape != src.shape or not np.array_equal(tile, src):
            bad += 1
    n = len(files)
    rows = tex.shape[0] // th
    empty_ok = True
    for i in range(n, ncols * rows):
        c, r = i % ncols, i // ncols
        if tex[r * th:(r + 1) * th, c * tw:(c + 1) * tw].any():
            empty_ok = False
    status = 'ok' if bad == 0 and empty_ok else 'FEHLER'
    print(f'{name}: {tex.shape[1]}x{tex.shape[0]} {tex.shape[2]} Kanaele {tbits} Bit, {ncols}x{rows}, '
          f'{n} Kacheln, abweichend {bad}, leere Felder {"ok" if empty_ok else "nicht leer"} -> {status}')
    if status != 'ok':
        failed += 1


check('rgba8', make('rgba8', 10, 37, 23, 'rgba8'))
check('rgba8_3spalten', os.path.join(root, 'rgba8'), cols=3)
check('rgb8', make('rgb8', 7, 50, 40, 'rgb8'))
check('rgba16', make('rgba16', 5, 31, 17, 'rgba16'))
check('rgb16', make('rgb16', 6, 20, 20, 'rgb16'))
check('gray16', make('gray16', 4, 25, 13, 'gray16'))
check('gray8', make('gray8', 9, 16, 16, 'gray8'))
check('gray4', make('gray4', 5, 15, 9, 'gray4'))
check('palette', make('palette', 11, 24, 24, 'pal'))
check('natuerlich', make('natuerlich', 12, 8, 8, 'rgba8', names=[f'f{i + 1}' for i in range(12)]))
check('gross', make('gross', 3, 1500, 900, 'rgba8'))
mix = make('mix', 3, 10, 10, 'rgb8')
write(os.path.join(mix, 'frame_0009.png'), 11, 10, 3, 8, rand_rows(11, 10, 3, 255), greyscale=False)
check('groesse_abweichend', mix, expect_fail=True)
fm = make('format', 3, 10, 10, 'rgb8')
write(os.path.join(fm, 'frame_0009.png'), 10, 10, 4, 8, rand_rows(10, 10, 4, 255), alpha=True, greyscale=False)
check('format_abweichend', fm, expect_fail=True)

print('FEHLGESCHLAGEN:', failed if failed else 'keine')
sys.exit(1 if failed else 0)
