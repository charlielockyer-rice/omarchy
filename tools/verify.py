"""Verify omarchy-hyprland-workspace-backgrounds in a disposable headless Hyprland.

  python3 tools/verify.py build/workspace-backgrounds.so [--only NAME ...]

Runs entirely headless (see headless.py): no window opens anywhere, and the
running desktop session is never contacted. Slides and swipes are checked on
every frame Hyprland renders, read back from the GPU by a test-only probe
plugin, not on a few sampled screenshots.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parent))
import headless  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('library', type=Path)
parser.add_argument('--helper', type=Path, help='workspace-backgrounds-prepare (default: next to the library)')
parser.add_argument('--artifacts', type=Path, default=Path.home() / '.cache/omarchy-hyprland-workspace-backgrounds-verification')
parser.add_argument('--only', nargs='*', default=[])
parser.add_argument('--scale', type=float, default=1)
parser.add_argument('--legacy', action='store_true', help='drive the 0.1 plugin (one picture per workspace id, decoded in Hyprland), to show which checks it fails')
args = parser.parse_args()
library = args.library.resolve()
helper = (args.helper or library.parent / 'workspace-backgrounds-prepare').resolve()
art = args.artifacts
shutil.rmtree(art, ignore_errors=True)
art.mkdir(parents=True)

W, H = 1600, 900
GAP = 120
SLIDE = 1.5  # seconds
MON = 'TEST-1'
C = {
  'base': (0x20, 0x30, 0x40), 'window': (0xff, 0x00, 0xff),
  'ws1': (0xc0, 0x39, 0x2b), 'ws2': (0x24, 0x71, 0xa3), 'ws3': (0x1e, 0x84, 0x49), 'ws4': (0xd4, 0xac, 0x0d), 'ws5': (0x8e, 0x44, 0xad),
  'ws6': (0xe6, 0x7e, 0x22), 'new1': (0x00, 0xc8, 0xc8), 'new2': (0xc8, 0xc8, 0x00), 'new3': (0x64, 0x64, 0xff), 'spill': (0x00, 0xff, 0x00),
}

shim, probe = headless.build(art)
pictures = art / 'pictures'
pictures.mkdir()


def solid(name, color=None, size='1920x1080'):
  path = pictures / f'{name}.png'
  rgb = C[color or name]
  subprocess.run(['magick', '-size', size, f'xc:#{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}', str(path)], check=True)
  return path


P = {n: solid(n) for n in ('ws1', 'ws2', 'ws3', 'ws4', 'ws5', 'ws6', 'new1', 'new2', 'new3')}
# A 3:1 picture, green at both ends and red across the 16:9 middle that
# covering a 16:9 screen keeps: green on screen means it spilled or stretched.
P['wide'] = pictures / 'wide.png'
subprocess.run(['magick', '-size', '1200x2160', 'xc:#00ff00', '-size', '4080x2160', 'xc:#c0392b', '-size', '1200x2160', 'xc:#00ff00', '+append', str(P['wide'])], check=True)
# Top half red, bottom half blue: which way up it lands on a rotated monitor.
P['updown'] = pictures / 'updown.png'
subprocess.run(['magick', '-size', '1920x540', 'xc:#c0392b', '-size', '1920x540', 'xc:#2471a3', '-append', str(P['updown'])], check=True)
P['broken'] = pictures / 'broken.png'
P['broken'].write_text('not an image')
# Slow to decode, so a newer set can arrive while it is still being prepared.
P['slow1'] = pictures / 'slow1.png'
P['slow2'] = pictures / 'slow2.png'
for p in (P['slow1'], P['slow2']):
  subprocess.run(['magick', '-size', '7000x7000', 'plasma:', '-depth', '8', str(p)], check=True)


def config(blur=False):
  return f'''
hl.monitor({{ output = "", mode = "{W}x{H}@60", position = "0x0", scale = {args.scale} }})
hl.monitor({{ output = "TEST-2", mode = "1280x720@60", position = "{W}x0", scale = 1 }})
hl.config({{
  general = {{ gaps_in = 0, gaps_out = {GAP}, border_size = 0, gaps_workspaces = 0 }},
  decoration = {{ rounding = 0, shadow = {{ enabled = false }}, blur = {{ enabled = {str(blur).lower()}, size = 6, passes = 2, new_optimizations = true, xray = false }} }},
  misc = {{ disable_hyprland_logo = true, disable_splash_rendering = true, background_color = "rgb(203040)" }},
  input = {{ follow_mouse = 0 }},
  gestures = {{ workspace_swipe_distance = 300 }},
  xwayland = {{ enabled = false }},
  debug = {{ damage_tracking = 0, disable_logs = false, enable_stdout_logs = true }},
}})
hl.curve("linear", {{ type = "bezier", points = {{ {{ 0, 0 }}, {{ 1, 1 }} }} }})
hl.animation({{ leaf = "workspaces", enabled = true, speed = {SLIDE * 10}, bezier = "linear", style = "slide" }})
hl.animation({{ leaf = "fadeIn", enabled = true, speed = 3, bezier = "linear" }})
'''


def near(a, b, tolerance=6):
  return all(abs(p - q) <= tolerance for p, q in zip(a, b))


def runs(row, tolerance=6):
  out = []
  for x, pixel in enumerate(row):
    name = next((k for k, c in C.items() if near(pixel, c, tolerance)), '?')
    if out and out[-1][0] == name:
      out[-1][2] = x
    else:
      out.append([name, x, x])
  return [r for r in out if r[0] != '?']


def kinds(row):
  return [r[0] for r in runs(row)]


def edge(row, left, right):
  stretches = runs(row)
  for a, b in zip(stretches, stretches[1:]):
    if a[0] == left and b[0] == right:
      return b[1]
  return None


class Suite:
  def __init__(self, blur=False, name='main'):
    self.h = headless.Hyprland(config(blur), art / name, shim)
    self.h.ctl('output', 'create', 'headless', MON)
    for _ in range(100):
      if self.h.json('monitors'):
        break
      time.sleep(.05)
    self.h.ctl('plugin', 'load', str(probe))
    # The cursor out of the rows the checks read.
    self.h.ctl('dispatch', f'hl.dsp.cursor.move({{ x = {W - 1}, y = {H - 1} }})')
    self.scale = self.h.json('monitors')[0]['scale']
    self.gap = round(GAP * self.scale)
    self.frames_n = 0

  def load(self):
    self.h.ctl('plugin', 'load', str(library))
    if not args.legacy:
      self.h.eval(f'hl.plugin.workspace_backgrounds.configure({{ helper = "{helper}" }})')

  def assign(self, mapping, preload=(1, 2, 3, 4, 5), monitor=MON):
    if args.legacy:
      body = ', '.join(f'[{k}] = "{v}"' for k, v in mapping.items())
      self.h.eval(f'hl.plugin.workspace_backgrounds.set({{ {body} }})')
      return
    entries = ', '.join(f'{{ workspace = {k}, path = "{v}", monitor = "{monitor}", preload = {str(k in preload).lower()} }}' for k, v in mapping.items())
    self.h.eval(f'hl.plugin.workspace_backgrounds.set({{ {entries} }})')

  def state(self):
    return self.h.json('workspacebackgrounds')

  def settle(self, timeout=20):
    """Until the plugin has nothing left to do: no set waiting, no helper, read or upload."""
    if args.legacy:
      time.sleep(2.5)
      return {}
    end = time.time() + timeout
    while time.time() < end:
      s = self.state()
      if not s['pending'] and s['helpers'] == 0 and s['loading'] == 0 and s['uploads_waiting'] == 0:
        time.sleep(.15)
        return s
      time.sleep(.05)
    raise AssertionError(f'plugin never settled: {json.dumps(self.state())[:600]}')

  def go(self, ws):
    self.h.ctl('dispatch', f'hl.dsp.focus({{ workspace = {ws} }})')

  def active(self):
    return [m for m in self.h.json('monitors') if m['name'] == MON][0]['activeWorkspace']['id']

  def wait_still(self):
    time.sleep(SLIDE + .3)

  def record(self, rows):
    self.frames_n += 1
    path = self.h.workdir / f'frames-{self.frames_n}.jsonl'
    self.h.eval(f'hl.plugin.probe.record("{path}", {{ {", ".join(str(r) for r in rows)} }})')
    return path

  def stop(self, path):
    self.h.eval('hl.plugin.probe.record()')
    return headless.read_frames(path)

  def shot(self, name, output=MON):
    return self.h.screenshot(self.h.workdir / f'{name}.ppm', output)

  def swipe(self, call):
    self.h.eval(f'hl.plugin.probe.swipe_{call}')


results = []


def test(fn):
  if args.only and fn.__name__ not in args.only:
    return fn
  start = time.time()
  try:
    note = fn() or ''
    results.append((fn.__name__, 'PASS', note, time.time() - start))
    print(f'PASS  {fn.__name__}  {note}', flush=True)
  except Exception as e:
    results.append((fn.__name__, 'FAIL', f'{type(e).__name__}: {e}', time.time() - start))
    print(f'FAIL  {fn.__name__}  {type(e).__name__}: {e}', flush=True)
    traceback.print_exc()
  return fn


s = Suite()
B5 = {i: P[f'ws{i}'] for i in range(1, 6)}
top = lambda f: f['rows'][s.gap // 2]  # noqa: E731
mid = lambda f: f['rows'][H // 2]  # noqa: E731
try:

  @test
  def load_reload_unload():
    for _ in range(3):
      s.load()
      s.assign({1: P['ws1'], 2: P['ws2']})
      s.h.ctl('reload')
      time.sleep(.2)
      assert s.h.alive()
      s.h.ctl('plugin', 'unload', str(library))
      time.sleep(.1)
    assert s.h.alive()
    return '3 rounds'

  @test
  def preloads_slots_one_to_five():
    s.load()
    s.assign(B5)
    st = s.settle()
    by = {w['workspace']: w for w in st['workspaces']}
    assert all(by[i]['ready'] and by[i]['level'] == 'full' for i in range(1, 6)), st['workspaces']
    assert not by[3]['exists'] and len(st['textures']) == 5, st
    commits = st['commits']
    s.h.ctl('reload')
    time.sleep(.5)
    f = s.shot('after-reload')
    assert kinds(f.row(H // 2)) == ['ws1'], runs(f.row(H // 2))
    assert s.state()['commits'] == commits, 'a reload must not prepare anything again'
    return f'5 textures, {st["bytes"] / 1e6:.1f} MB, upload {st["max_upload_ms"]:.1f} ms max; reload re-prepares nothing'

  # A window on workspace 2, so it outlives being left.
  s.go(2)
  s.wait_still()
  s.h.spawn_window('owb-window')
  time.sleep(1)

  def slide_frames(frames, left, right, window_side):
    """Every frame of a slide: the picture edge and the window edge on the same
    workspace stay exactly one gap apart, the edge moves one way, and the
    arriving picture starts exactly at the arriving workspace's render offset."""
    checked, edges = 0, []
    for f in frames:
      b = edge(top(f), left, right)
      if b is None:
        continue
      if window_side == 'right':
        w = edge(mid(f), right, 'window')
        if w is not None:
          assert abs(w - b - s.gap) <= 1, (f['frame'], b, w)
      else:
        w = edge(mid(f), 'window', left)
        if w is not None:
          assert abs(b - (w + s.gap)) <= 1 or w is None, (f['frame'], b, w)
      off = f['offset'][0] * s.scale
      expect = round(off) if off > 0 else round(W + off)
      assert abs(b - expect) <= 1, ('picture edge off its workspace', f['frame'], b, f['offset'])
      edges.append(b)
      checked += 1
    return checked, edges

  @test
  def slide_every_frame():
    path = s.record([s.gap // 2, H // 2])
    s.go(1)
    s.wait_still()
    frames = s.stop(path)
    moving = [f for f in frames if f['offset'][0] != 0]
    checked, edges = slide_frames(frames, 'ws1', 'ws2', 'right')
    assert checked >= len(moving) - 1 and checked > 40, (checked, len(moving))
    assert edges == sorted(edges), 'edge must only move one way'
    for f in moving:
      assert 'base' not in kinds(top(f)), ('blank during the slide', f['frame'])
    return f'{checked} of {len(frames)} frames checked'

  @test
  def empty_workspace_slides_out():
    # 1 is empty: Hyprland destroys it as it is left; its picture must leave with it.
    path = s.record([s.gap // 2, H // 2])
    s.go(2)
    s.wait_still()
    frames = s.stop(path)
    checked, edges = slide_frames(frames, 'ws1', 'ws2', 'right')
    moving = [f for f in frames if f['offset'][0] != 0]
    assert checked >= len(moving) - 1 and checked > 40, (checked, len(moving))
    assert edges == sorted(edges, reverse=True)
    return f'{checked} frames'

  @test
  def jump_one_to_four_is_instant():
    s.go(1)
    s.wait_still()
    before = s.state()
    path = s.record([s.gap // 2, H // 2])
    s.go(4)
    s.wait_still()
    frames = s.stop(path)
    after = s.state()
    moving = [f for f in frames if f['active'] == 4 and f['offset'][0] != 0]
    assert len(moving) > 40, len(moving)
    first = moving[0]
    assert kinds(top(first)) in (['ws1', 'ws4'], ['ws1']), runs(top(first))
    for f in frames:
      if f['active'] == 4:
        k = kinds(top(f))
        assert 'base' not in k and set(k) <= {'ws1', 'ws4'}, ('blank or faded frame', f['frame'], runs(top(f)))
        # Exact colours: no fade-in on arrival.
        assert sum(r[2] - r[1] + 1 for r in runs(top(f), 2)) >= W - 2, ('not exact', f['frame'])
    assert after['uploads'] == before['uploads'], 'everything it needed was already on the GPU'
    return f'{len(moving)} frames, no blank, no fade, no upload'

  @test
  def interrupted_switch():
    # 4 -> 2 (empty? no: 2 has the window) ... use empty 3 and 5: 4 -> 5, interrupted by -> 3? Use 1 -> 3 -> 5.
    s.go(1)
    s.wait_still()
    path = s.record([s.gap // 2, H // 2])
    s.go(3)
    time.sleep(SLIDE * .4)
    s.go(5)
    s.wait_still()
    frames = s.stop(path)
    # Each emptied workspace's picture keeps sliding left until it is off
    # screen; none vanishes mid-screen when the next switch starts.
    for name in ('ws1', 'ws3'):
      seen = [i for i, f in enumerate(frames) if name in kinds(top(f))]
      if name == 'ws3':
        seen = [i for i in seen if frames[i]['active'] in (3, 5)]
      assert seen, name
      assert seen == list(range(seen[0], seen[-1] + 1)), (name, 'vanished and came back', seen[:5], seen[-5:])
      rights = [[r for r in runs(top(frames[i])) if r[0] == name][0][2] for i in seen]
      if name == 'ws1':
        assert rights == sorted(rights, reverse=True), (name, rights[:10])
      last = frames[seen[-1]]
      nxt = frames[seen[-1] + 1] if seen[-1] + 1 < len(frames) else None
      step = max(abs(a - b) for a, b in zip(rights, rights[1:])) if len(rights) > 1 else W
      assert rights[-1] <= step + 2 or (nxt and nxt['offset'][0] == 0), (name, 'vanished on screen at x', rights[-1], 'step', step)
    for f in frames:
      if f['active'] == 5 and f['offset'][0] > 0:
        b = edge(top(f), [k for k in kinds(top(f)) if k != 'ws5'][-1] if len(kinds(top(f))) > 1 else 'ws5', 'ws5')
        if b is not None:
          assert abs(b - round(f['offset'][0] * s.scale)) <= 1, ('ws5 off its offset', f['frame'], b, f['offset'])
    f = s.shot('interrupted-end')
    assert kinds(f.row(H // 2)) == ['ws5']
    return f'{len(frames)} frames'

  @test
  def swipe_cancel_and_commit():
    s.go(2)
    s.wait_still()
    # Toward a workspace that does not exist yet, let go short: slides back.
    path = s.record([s.gap // 2, H // 2])
    s.swipe('begin()')
    for step in range(1, 21):
      s.swipe(f'update({5 * step})')
    s.swipe('finish()')
    s.wait_still()
    frames = s.stop(path)
    assert s.active() == 2
    checked = 0
    for f in frames:
      b = edge(top(f), 'ws2', 'ws3')
      w = edge(mid(f), 'window', 'ws2')
      if b is not None and w is not None:
        assert abs(b - (w + s.gap)) <= 1, (f['frame'], b, w)
        checked += 1
    assert checked > 20, checked
    # Past halfway: Hyprland creates the workspace on release; its picture rides in.
    path = s.record([s.gap // 2, H // 2])
    s.swipe('begin()')
    s.swipe('update(100)')
    time.sleep(.1)
    s.swipe('update(200)')
    time.sleep(.1)
    s.swipe('finish()')
    s.wait_still()
    frames2 = s.stop(path)
    assert s.active() == 3
    checked2, edges = 0, []
    for f in frames2:
      b = edge(top(f), 'ws2', 'ws3')
      w = edge(mid(f), 'window', 'ws2')
      if b is not None:
        edges.append(b)
        if w is not None:
          assert abs(b - (w + s.gap)) <= 1, (f['frame'], b, w)
          checked2 += 1
      assert 'base' not in kinds(top(f)), ('blank during the swipe', f['frame'], runs(top(f)))
    assert checked2 > 10 and edges[-1] < edges[0], (checked2, edges[:3], edges[-3:])
    return f'{checked} frames let go short, {checked2} committed'

  @test
  def slidefade_fades_the_emptied_one():
    s.h.eval(f'hl.animation({{ leaf = "workspaces", enabled = true, speed = {SLIDE * 10}, bezier = "linear", style = "slidefade 50%" }})')
    s.go(2)  # leaving 3, which is empty
    time.sleep(SLIDE / 2)
    f = s.shot('slidefade-mid')
    px = f.at(W - W // 20, s.gap // 2)
    s.wait_still()
    s.h.eval(f'hl.animation({{ leaf = "workspaces", enabled = true, speed = {SLIDE * 10}, bezier = "linear", style = "slide" }})')
    assert all(min(b, c) + 8 < p < max(b, c) - 8 for p, b, c in zip(px, C['base'], C['ws3']) if abs(b - c) > 40), px
    return str(px)

  @test
  def theme_switch_mid_slide():
    s.go(1)
    s.wait_still()
    before = s.state()
    path = s.record([s.gap // 2, H // 2])
    s.go(2)
    time.sleep(SLIDE * .3)
    s.assign({1: P['new1'], 2: P['new2'], 3: P['new3'], 4: P['ws4'], 5: P['ws5']})
    time.sleep(SLIDE * .9 + 1.5)
    frames = s.stop(path)
    after = s.state()
    first_new = next((f for f in frames if {'new1', 'new2'} & set(kinds(top(f)))), None)
    assert first_new is not None, 'the new set never showed'
    assert first_new['offset'] == [0.0, 0.0], ('new pictures appeared mid-slide', first_new['frame'], first_new['offset'])
    assert after['commits'] == before['commits'] + 1
    assert after['uploads_in_motion'] == before['uploads_in_motion'], 'uploaded during a slide'
    f = s.shot('theme-switched')
    assert kinds(f.row(s.gap // 2)) == ['new2'], runs(f.row(s.gap // 2))
    return f'swapped at frame {first_new["frame"]} after the slide; {after["uploads"] - before["uploads"]} uploads, none in motion'

  @test
  def newer_set_cancels_older():
    before = s.state()
    path = s.record([s.gap // 2])
    s.assign({1: P['slow1'], 2: P['slow2'], 3: P['new3'], 4: P['ws4'], 5: P['ws5']})
    time.sleep(.05)
    s.assign({1: P['ws1'], 2: P['ws2'], 3: P['ws3'], 4: P['ws4'], 5: P['ws5']})
    st = s.settle(30)
    time.sleep(1)  # the fade from the theme switch's pictures
    frames = s.stop(path)
    assert st['helpers_stopped'] > before['helpers_stopped'], 'the obsolete helpers kept running'
    names = {k for f in frames for k in kinds(top(f))}
    assert names <= {'new2', 'ws2'}, names
    f = s.shot('newer-set')
    assert kinds(f.row(s.gap // 2)) == ['ws2']
    return f'{st["helpers_stopped"] - before["helpers_stopped"]} helpers stopped, obsolete pictures never drawn'

  @test
  def unassigned_shows_layer():
    s.assign({1: P['ws1'], 3: P['ws3'], 4: P['ws4'], 5: P['ws5']})
    s.settle()
    time.sleep(1.5)
    f = s.shot('unassigned')
    assert kinds(f.row(s.gap // 2)) == ['base'], runs(f.row(s.gap // 2))

  @test
  def failed_picture_keeps_fallback_across_mode_change():
    s.assign(B5)
    s.settle()
    time.sleep(1.2)
    started = s.state()['helpers_started']
    s.assign({**B5, 2: P['broken']})
    st = s.settle()
    w2 = [w for w in st['workspaces'] if w['workspace'] == 2][0]
    assert w2['failed'] and w2['drawn'] == str(P['ws2']) and w2['error'], w2
    s.h.eval(f'hl.monitor({{ output = "{MON}", mode = "1280x720@60", position = "0x0", scale = {args.scale} }})')
    time.sleep(.5)
    st = s.settle()
    f = s.shot('fallback-1280')
    assert (f.width, f.height) == (1280, 720)
    assert kinds(f.row(5)) == ['ws2'], runs(f.row(5))
    broken_runs = st['helpers_started'] - started
    s.h.eval(f'hl.monitor({{ output = "{MON}", mode = "{W}x{H}@60", position = "0x0", scale = {args.scale} }})')
    time.sleep(.5)
    s.assign(B5)
    s.settle()
    # One helper for the broken file; the rest are ws1-5 at the new size.
    return f'broken prepared once, fallback kept at 1280x720 ({broken_runs} helper runs)'

  @test
  def wide_picture_is_cropped_not_stretched():
    s.assign({**B5, 1: P['wide']})
    s.settle()
    s.go(1)
    path = s.record([s.gap // 2, H // 2])
    s.go(2)
    s.wait_still()
    frames = s.stop(path)
    for f in frames:
      assert 'spill' not in kinds(top(f)), ('spilled', f['frame'])
    f = s.shot('wide')
    assert kinds(f.row(5)) == ['ws2']
    s.go(1)
    s.wait_still()
    f = s.shot('wide-settled')
    assert kinds(f.row(5)) == ['ws1'], runs(f.row(5))
    s.assign(B5)
    s.settle()

  @test
  def same_path_new_picture():
    same = pictures / 'same.png'
    shutil.copy(P['ws3'], same)
    s.assign({**B5, 1: same})
    s.settle()
    time.sleep(1.2)
    assert kinds(s.shot('same-before').row(5)) == ['ws3']
    time.sleep(.02)
    shutil.copy(P['ws4'], same)
    s.assign({**B5, 1: same})
    s.settle()
    time.sleep(1.2)
    assert kinds(s.shot('same-after').row(5)) == ['ws4']
    s.assign(B5)
    s.settle()

  @test
  def budget_shrinks_instead_of_failing():
    s.h.eval('hl.plugin.workspace_backgrounds.configure({ budget_mb = 20 })')
    st = s.settle()
    levels = {w['workspace']: w['level'] for w in st['workspaces']}
    assert st['bytes'] <= 20 << 20, st['bytes']
    assert levels[s.active()] == 'full' and 'half' in levels.values(), levels
    s.h.eval('hl.plugin.workspace_backgrounds.configure({ budget_mb = 3 })')
    st2 = s.settle()
    levels2 = {w['workspace']: w['level'] for w in st2['workspaces']}
    assert st2['bytes'] <= 3 << 20 and 'quarter' in levels2.values(), (st2['bytes'], levels2)
    f = s.shot('quarter')
    assert kinds(f.row(5)) == [f'ws{s.active()}'], runs(f.row(5))
    s.h.eval('hl.plugin.workspace_backgrounds.configure({ budget_mb = 384 })')
    st3 = s.settle()
    assert all(w['level'] == 'full' for w in st3['workspaces'])
    return f'20 MB budget: {st["bytes"] / 1e6:.1f} MB {sorted(levels.items())}; 3 MB budget: {st2["bytes"] / 1e6:.2f} MB'

  @test
  def monitor_added_and_removed():
    s.h.ctl('output', 'create', 'headless', 'TEST-2')
    time.sleep(.6)
    mons = {m['name']: m for m in s.h.json('monitors')}
    ws2 = mons['TEST-2']['activeWorkspace']['id']
    s.assign({**B5, ws2: P['ws6'], 12: P['new3']}, preload=(1, 2, 3, 4, 5, ws2, 12), monitor=MON)
    # The second monitor's workspaces belong to it.
    entries = ', '.join([f'{{ workspace = {k}, path = "{v}", monitor = "{MON}", preload = true }}' for k, v in B5.items()] +
                        [f'{{ workspace = {ws2}, path = "{P["ws6"]}", monitor = "TEST-2", preload = true }}', f'{{ workspace = 12, path = "{P["new3"]}", monitor = "TEST-2", preload = true }}'])
    s.h.eval(f'hl.plugin.workspace_backgrounds.set({{ {entries} }})')
    st = s.settle()
    sizes = {(t['width'], t['height']) for t in st['textures']}
    assert (1280, 720) in sizes and (W, H) in sizes, sizes
    time.sleep(1)  # its fade in
    f = s.shot('second-monitor', 'TEST-2')
    assert (f.width, f.height) == (1280, 720) and kinds(f.row(5)) == ['ws6'], runs(f.row(5))
    s.h.ctl('output', 'remove', 'TEST-2')
    time.sleep(.8)
    assert s.h.alive()
    st = s.settle()
    assert s.h.alive() and all((t['width'], t['height']) == (W, H) for t in st['textures']), [(t['width'], t['height']) for t in st['textures']]
    s.h.ctl('output', 'create', 'headless', 'TEST-2')
    time.sleep(.8)
    st = s.settle()
    s.h.ctl('output', 'remove', 'TEST-2')
    time.sleep(.5)
    s.assign(B5)
    s.settle()
    assert s.h.alive()
    return f'textures at {sorted(sizes)}; removed, re-added, removed'

  @test
  def rotated_monitor_is_upright():
    s.assign({**B5, s.active(): P['updown']})
    s.settle()
    s.h.eval(f'hl.monitor({{ output = "{MON}", mode = "{W}x{H}@60", position = "0x0", scale = {args.scale}, transform = 1 }})')
    time.sleep(.6)
    st = s.settle()
    time.sleep(1.2)
    f = s.shot('rotated')
    sizes = {(t['width'], t['height']) for t in st['textures']}
    assert (H, W) in sizes, sizes
    assert (f.width, f.height) == (H, W), (f.width, f.height)
    assert near(f.at(H // 2, 5), C['ws1']) and near(f.at(H // 2, W - 5), C['ws2']), (f.at(H // 2, 5), f.at(H // 2, W - 5))
    s.h.eval(f'hl.monitor({{ output = "{MON}", mode = "{W}x{H}@60", position = "0x0", scale = {args.scale}, transform = 0 }})')
    time.sleep(.5)
    s.assign(B5)
    s.settle()
    return f'portrait {H}x{W} texture, red on top'

  @test
  def overview_tile_draws_that_workspace():
    s.go(1)
    s.wait_still()
    assert s.state()['overview_hook']
    # renderWorkspace's geometry is scaled with its offset: this tile lands
    # at (300, 0), 400x225. Drawn into the frame itself, the nested render
    # clears what was under it, so only the tile is checked.
    s.h.eval('hl.plugin.probe.tile(2, 1200, 0, 400, 225)')
    time.sleep(.4)
    f = s.shot('overview-tile')
    s.h.eval('hl.plugin.probe.tile()')
    assert near(f.at(500, 100), C['ws2']), ('tile shows', f.at(500, 100))
    return 'a tile of workspace 2, drawn while 1 is active, shows 2\'s picture'

  @test
  def corrupt_pictures_through_plugin():
    corpus = art / 'corpus'
    subprocess.run([sys.executable, str(Path(__file__).parent / 'corpus.py'), str(helper), '--keep', str(corpus)], check=False, capture_output=True)
    bad = ['huge-header.png', 'huge-sof.jpg', 'truncated.png', 'corrupt-idat.png', 'random.png', 'fifo.png', 'zero.png', 'empty.jpg', 'huge-vp8x.webp']
    s.assign(B5)
    s.settle()
    rss = s.h.rss_kb()
    mapping = {i + 1: corpus / name for i, name in enumerate(bad[:5])}
    mapping.update({6 + i: corpus / name for i, name in enumerate(bad[5:])})
    s.assign(mapping, preload=tuple(range(1, 11)))
    st = s.settle(60)
    assert s.h.alive()
    failed = [w for w in st['workspaces'] if w['failed']]
    assert len(failed) == len(bad), [(w['workspace'], w['error']) for w in st['workspaces']]
    f = s.shot('corrupt')
    assert kinds(f.row(5)) == [f'ws{s.active()}'], 'keeps the picture it had'
    s.assign(B5)
    s.settle()
    return f'{len(bad)} refused, compositor RSS {rss // 1024} -> {s.h.rss_kb() // 1024} MB'

  @test
  def clear_and_unload():
    s.assign({})
    time.sleep(.5)
    assert kinds(s.shot('cleared').row(5)) == ['base']
    s.assign(B5)
    s.settle()
    s.h.ctl('plugin', 'unload', str(library))
    time.sleep(.3)
    assert kinds(s.shot('unloaded').row(5)) == ['base'] and s.h.alive()

finally:
  s.h.stop()


# ----------------------------------------------------------------- blur on

@test
def blur_follows_the_workspace():
  """Glassy: see-through windows blur what is behind them, from a cached blur
  of the background. Two translucent black windows, on workspaces with red and
  blue pictures: inside the one on the blue workspace, every frame of the
  switch and after it must be blue, never the red of the one left."""
  b = Suite(blur=True, name='blur')
  try:
    b.load()
    b.assign({1: P['ws1'], 2: P['ws2']})
    b.settle()
    b.go(2)
    b.wait_still()
    b.h.spawn_window('owb-glass-2', color='000000', alpha=0.5, blur=True)
    b.go(1)
    b.wait_still()
    b.h.spawn_window('owb-glass-1', color='000000', alpha=0.5, blur=True)
    time.sleep(1)
    # Hyprland itself refreshes the cached blur on a blur setting change: so
    # the cache now holds workspace 1's red picture, blurred.
    b.h.eval('hl.config({ decoration = { blur = { size = 6 } } })')
    time.sleep(.3)
    path = b.record([H // 2])
    b.go(2)
    b.wait_still()
    frames = b.stop(path)
    bad = []
    checked = 0
    for f in frames:
      if f['active'] != 2:
        continue
      # The window on 2 sits from its workspace's left edge + gap.
      off = round(f['offset'][0] * b.scale)
      x = off + b.gap + 200
      if x >= W - b.gap - 20:
        continue
      r, g, bl = f['rows'][H // 2][x]
      checked += 1
      if not bl > r + 20:
        bad.append((f['frame'], x, (r, g, bl)))
    shot = b.shot('blur-settled')
    r, g, bl = shot.at(W // 2, H // 2)
    assert bl > r + 20, ('settled: the window shows the picture of the workspace left, blurred', (r, g, bl))
    assert not bad, f'{len(bad)} of {checked} frames show the old picture blurred, e.g. {bad[:3]}'
    assert checked > 30, checked
    return f'{checked} frames, window interior blue every frame ({(r, g, bl)} settled)'
  finally:
    b.h.stop()


print()
failed = [r for r in results if r[1] != 'PASS']
for name, status, note, secs in results:
  print(f'{status}  {name:<48} {secs:5.1f}s  {note}')
print(f'\n{len(results) - len(failed)} passed, {len(failed)} failed; artifacts in {art}')
sys.exit(1 if failed else 0)
