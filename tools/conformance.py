#!/usr/bin/env python3
"""
conformance.py - does the recompiled Epic Pinball still do what the original does?

    py tools/conformance.py              # run every scenario (needs original/ and a build)
    py tools/conformance.py --update     # accept the current count as the baseline

There is no reference build to diff against frame by frame (the original runs
only under DOSBox), so the ground truth is behaviour the original visibly has:
PINBALL.EXE boots to its menu, hands each table to the right EXE, the table
draws its playfield and scoreboard, scrolls as the ball moves, plays music and
effects through the Sound Blaster, and nothing ever lands on an address with
no lifted code. Each scenario drives the game headless with scripted keys and
checks those things. See docs/conformance.md.

The game data is not redistributable, so on a machine without it (CI) every
scenario is reported as SKIP and the run passes. A count below the committed
baseline (tests/conformance_baseline.json) fails, so a regression cannot land
quietly; --update records a new, higher figure.
"""
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, 'tests', 'conformance_baseline.json')
EXE = os.environ.get('EPIC_EXE') or (os.path.join(ROOT, 'build', 'Release', 'epicpinball.exe') if os.name == 'nt'
                                     else os.path.join(ROOT, 'build-linux', 'epicpinball'))
DATA = os.path.join(ROOT, 'original')
TABLES = 13


def read_bmp(path):
    """The runtime's own BMPs: 32-bit, top-down. -> (w, h, list of 0xRRGGBB)"""
    d = open(path, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    h = abs(h)
    px = struct.unpack_from(f'<{w * h}I', d, 54)
    return w, h, [p & 0xFFFFFF for p in px]


def rows(img, y0, y1):
    w, h, px = img
    return px[y0 * w:min(y1, h) * w]


def bright(px, level=120):
    return sum(1 for p in px if max(p >> 16, (p >> 8) & 255, p & 255) > level)


def wav_rms(path):
    d = open(path, 'rb').read()
    n = (len(d) - 44) // 2
    if n <= 0:
        return 0.0
    s = struct.unpack_from(f'<{n}h', d, 44)
    sub = s[::max(1, n // 200000)]
    return (sum(v * v for v in sub) / len(sub)) ** 0.5


PLAY_MS = 30000          # of play after the table is up


def table_keys(n):
    """PINBALL's menu to table n, then a game (tools/discover.py plays the
    same script). BEGIN, n-1 short DOWNs (a held key auto-repeats),
    ENTER on the table, ENTER on PLAY GAME; on the table SPACE starts a game,
    then a long SPACE (the plunger) every 6 s -- each new ball waits in the
    lane for it -- with both flippers in between.
    -> (keys, ms the table is up, ms to run for)"""
    keys, t = ['12500:ENTER'], 13500
    for _ in range(n - 1):
        keys.append(f'{t}:DOWN+40')
        t += 500
    keys += [f'{t}:ENTER', f'{t + 2000}:ENTER']
    p = t + 9000
    keys.append(f'{p}:SPACE')
    end = p + PLAY_MS
    f = p + 2000
    while f < end - 3000:
        keys.append(f'{f}:SPACE+1500')
        for k in range(2):
            keys += [f'{f + 2000 + 1700 * k}:LSHIFT+150', f'{f + 2700 + 1700 * k}:RSHIFT+150']
        f += 6000
    return ','.join(keys), p, end


def run(out, seconds, keys, shots, extra=()):
    save = os.path.join(out, 'save')
    os.makedirs(save, exist_ok=True)
    cmd = [EXE, '--headless', '--trace', '--seconds', str(seconds), '--save', save,
           '--data', DATA, '--wav', os.path.join(out, 'audio.wav')]
    cmd += list(extra)
    if keys:
        cmd += ['--keys', keys]
    if shots:
        cmd += ['--shot-at', ','.join(f'{ms}:{os.path.join(out, n + ".bmp")}'     # in time order
                                      for n, ms in sorted(shots.items(), key=lambda kv: kv[1]))]
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=300)
        log, code = p.stderr.decode('latin-1'), p.returncode
    except subprocess.TimeoutExpired as e:
        log, code = (e.stderr or b'').decode('latin-1') + '\n[timeout]', -1
    open(os.path.join(out, 'run.log'), 'w', encoding='latin-1').write(log)
    return code, log


def misses(log):
    return [l for l in log.splitlines() if l.startswith('[miss]')]


def scenario_boot(out):
    code, log = run(out, 14, None, {'menu': 12000})
    img = read_bmp(os.path.join(out, 'menu.bmp')) if os.path.exists(os.path.join(out, 'menu.bmp')) else None
    return [
        ('boot: clean exit', code == 0),
        ('boot: no dispatch misses', not misses(log)),
        ('boot: SETUP check answered, sound drivers placed',
         'SETUP -cd answered' in log and 'MDRV004R.MUS live' in log and 'MDRV000R.MUS live' in log),
        ('boot: menu on screen', img is not None and len(set(img[2])) >= 8 and bright(img[2]) > 500),
    ]


def scenario_table(out, n):
    keys, play, end = table_keys(n)
    a, b = play + 12000, end - 500
    code, log = run(out, end / 1000, keys, {'a': a, 'b': b})
    checks = [(f'table {n}: clean exit', code == 0),
              (f'table {n}: PINBALL ran EP{n}.EXE', f'exec ep{n}.exe' in log.lower()),
              (f'table {n}: no dispatch misses', not misses(log))]
    imgs = [read_bmp(os.path.join(out, f'{k}.bmp')) if os.path.exists(os.path.join(out, f'{k}.bmp')) else None
            for k in ('a', 'b')]
    ok = all(i is not None and i[0] == 320 and i[1] == 240 for i in imgs)
    checks.append((f'table {n}: 320x240 playfield with 32+ colours',
                   ok and all(len(set(rows(i, 0, 220))) >= 32 for i in imgs)))
    # the score display: CRTC line compare, rows 221+ from VRAM address 0
    # (lit dots: Pot of Gold's are a dark green)
    checks.append((f'table {n}: scoreboard in the split screen', ok and bright(rows(imgs[1], 221, 240), 60) > 40))
    if ok:
        pa, pb = rows(imgs[0], 0, 220), rows(imgs[1], 0, 220)
        changed = sum(1 for x, y in zip(pa, pb) if x != y) / len(pa)
    else:
        changed = 0
    checks.append((f'table {n}: the view moves in play', changed > 0.02))
    checks.append((f'table {n}: SB16 stereo stream started', '[sb] SB16 stereo auto-init' in log))
    checks.append((f'table {n}: audio', wav_rms(os.path.join(out, 'audio.wav')) > 200))
    return checks


def scenario_remaster(out, n, plain):
    """The remaster's picture, on a table with each scoreboard height (1-8: 20
    rows, 9-13: 30). Whole table: the page the screen scrolls over, drawn from
    VRAM -- the frame the game shows at the same moment must be a band of it --
    with the scoreboard under it. The presenter (offscreen: Direct3D 11, or
    SDL2's software renderer on Linux): that picture letterboxed into 1280x720
    with the blur beside it."""
    keys, play, end = table_keys(n)
    t = end - 500                      # scenario_table's frame `b`, same script: the same moment
    w_dir, p_dir = os.path.join(out, 'whole'), os.path.join(out, 'present')
    for d in (w_dir, p_dir):
        os.makedirs(d, exist_ok=True)
    run(w_dir, end / 1000, keys, {'w': t}, ['--whole-table'])
    run(p_dir, end / 1000, keys, {'p': t}, ['--whole-table', '--present', '1280x720'])
    get = lambda d, k: read_bmp(os.path.join(d, k + '.bmp')) if os.path.exists(os.path.join(d, k + '.bmp')) else None
    whole, shown, pres = get(w_dir, 'w'), plain, get(p_dir, 'p')
    band = False
    if whole and shown and whole[0] == 320 and 400 <= whole[1] <= 440:
        view = rows(shown, 0, 200)                             # playfield on every table
        band = any(rows(whole, k, k + 200) == view for k in range(0, whole[1] - 200 - 19 + 1))
    dmd = whole is not None and bright(rows(whole, whole[1] - 19, whole[1]), 60) > 40
    pic = bars = False
    if pres and whole and pres[0] == 1280 and pres[1] == 720:
        pw = round(720 * 320 / whole[1])                       # square pixels: the page's own shape
        x0 = (1280 - pw) // 2
        mid = [pres[2][y * 1280 + x] for y in range(0, 720, 8) for x in range(x0 + 4, x0 + pw - 4, 8)]
        side = [pres[2][y * 1280 + x] for y in range(0, 720, 8) for x in range(0, x0 - 4, 8)]
        pic = len(set(mid)) >= 64
        bars = bright(side, 8) > len(side) // 2 and bright(side, 160) == 0     # lit, but dark
    return [(f'remaster table {n}: whole table is the page the screen scrolls over', band),
            (f'remaster table {n}: scoreboard under the whole table', dmd),
            (f'remaster table {n}: presenter letterboxes it at 1280x720', pic),
            (f'remaster table {n}: a dark blur of the picture beside it', bars)]


def scenario_determinism(out):
    """Headless time is deterministic: the same script twice, the same frame."""
    keys, play, end = table_keys(1)
    frames = []
    for k in ('x', 'y'):
        d = os.path.join(out, k)
        os.makedirs(d, exist_ok=True)
        run(d, end / 1000, keys, {'f': end - 1000})
        p = os.path.join(d, 'f.bmp')
        frames.append(open(p, 'rb').read() if os.path.exists(p) else None)
    return [('determinism: two runs, identical frames', frames[0] is not None and frames[0] == frames[1])]


def lift_checks():
    out = []
    gen = os.path.join(ROOT, 'work', 'gen')
    for name in sorted(os.listdir(gen)) if os.path.isdir(gen) else []:
        j = os.path.join(gen, name, 'lift.json')
        if os.path.exists(j):
            u = json.load(open(j)).get('unhandled', {})
            out.append((f'lift {name}: every instruction form handled', not u))
    return out


def main():
    update = '--update' in sys.argv
    if not os.path.exists(os.path.join(DATA, 'PINBALL.EXE')) or not os.path.exists(EXE):
        print('SKIP: no game data in original/ or no build -- conformance needs your own copy '
              '(README, Getting Started)')
        return 0
    results = lift_checks()
    tmp = os.path.join(ROOT, 'work', 'conformance')     # kept: the frames and logs explain a FAIL
    if True:
        def sub(name):
            d = os.path.join(tmp, name)
            os.makedirs(d, exist_ok=True)
            return d
        results += scenario_boot(sub('boot'))
        for n in range(1, TABLES + 1):
            results += scenario_table(sub(f'table{n}'), n)
            print(f'  table {n} done', file=sys.stderr)
        for n in (1, 9):
            b = os.path.join(tmp, f'table{n}', 'b.bmp')
            results += scenario_remaster(sub(f'remaster{n}'), n, read_bmp(b) if os.path.exists(b) else None)
        results += scenario_determinism(sub('det'))
    for name, ok in results:
        print(f'{"PASS" if ok else "FAIL"}  {name}')
    passed, total = sum(ok for _, ok in results), len(results)
    print(f'\nconformance: {passed}/{total}')
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {'passed': 0, 'total': 0}
    if update:
        json.dump({'passed': passed, 'total': total}, open(BASELINE, 'w'), indent=1)
        print(f'baseline updated: {passed}/{total}')
        return 0
    if passed < base['passed']:
        print(f'REGRESSION: {passed} passed, baseline {base["passed"]}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
