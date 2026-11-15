"""Verify omarchy-hyprland-workspace-backgrounds in an isolated nested Hyprland.

The candidate library is only ever loaded into the disposable nested compositor,
never into the parent session. Every check reads real frames back with grim.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('library', type=Path, help='built workspace-backgrounds.so to test')
parser.add_argument('--scale', type=float, default=1, help='nested monitor scale')
parser.add_argument('--artifacts', type=Path, default=Path.home() / '.cache/omarchy-hyprland-workspace-backgrounds-verification')
args = parser.parse_args()
args.library = args.library.resolve()
if not args.library.is_file():
  parser.error(f'{args.library} does not exist')
args.artifacts.mkdir(parents=True, exist_ok=True)

BASE = (0x20, 0x30, 0x40)
COLORS = {1: (0xc0, 0x39, 0x2b), 2: (0x24, 0x71, 0xa3), 3: (0x1e, 0x84, 0x49), 4: (0xd4, 0xac, 0x0d)}
WINDOW = (0xff, 0x00, 0xff)
GAPS_OUT = 120
SLIDE_SECONDS = 6

# Hyprland's socket path must fit in sun_path, so the runtime directory stays short.
with tempfile.TemporaryDirectory(prefix='owb-', dir=Path.home()) as scratch:
  scratch = Path(scratch)
  images = {}
  for n, rgb in COLORS.items():
    images[n] = scratch / f'ws{n}.png'
    subprocess.run(['magick', '-size', '3840x2160', f'xc:#{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}', str(images[n])], check=True)
  wide = scratch / 'wide.png'
  # A 3:1 picture, green at both ends and red across the 16:9 middle that
  # covering a 16:9 screen keeps: any green on screen means it spilled.
  subprocess.run(['magick', '-size', '1200x2160', 'xc:#00ff00', '-size', '4080x2160', 'xc:#c0392b', '-size', '1200x2160', 'xc:#00ff00', '+append', str(wide)], check=True)

  subprocess.run(['g++', '-std=c++23', '-shared', '-fPIC', '-fno-gnu-unique', '-O1', str(source / 'swipe.cpp'), '-o', str(scratch / 'swipe.so'),
                  *subprocess.check_output(['pkg-config', '--cflags', 'hyprland', 'pixman-1', 'libdrm'], text=True).split()], check=True)

  config = scratch / 'hyprland.lua'
  config.write_text(f'''
hl.monitor({{ output = "", mode = "1600x900@60", position = "0x0", scale = {args.scale} }})
hl.config({{
  general = {{ gaps_in = 0, gaps_out = {GAPS_OUT}, border_size = 0 }},
  decoration = {{ rounding = 0, shadow = {{ enabled = false }}, blur = {{ enabled = false }} }},
  misc = {{ disable_hyprland_logo = true, disable_splash_rendering = true, background_color = "rgb(203040)" }},
  input = {{ follow_mouse = 0 }},
  gestures = {{ workspace_swipe_distance = 300 }},
}})
hl.curve("linear", {{ type = "bezier", points = {{ {{ 0, 0 }}, {{ 1, 1 }} }} }})
hl.animation({{ leaf = "workspaces", enabled = true, speed = {SLIDE_SECONDS * 10}, bezier = "linear", style = "slide" }})
hl.animation({{ leaf = "fadeIn", enabled = true, speed = 10, bezier = "linear" }})
hl.window_rule({{ name = "test-window", match = {{ class = "owb-window" }}, float = false }})
''')

  parent_instances = json.loads(subprocess.check_output(['hyprctl', 'instances', '-j'], text=True))
  if len(parent_instances) != 1:
    raise RuntimeError('Verification expects exactly one parent compositor')
  parent_signature = parent_instances[0]['instance']
  parent_display = str(Path(os.environ['XDG_RUNTIME_DIR']) / parent_instances[0]['wl_socket'])
  run = scratch / 'run'
  run.mkdir(mode=0o700)
  env = dict(os.environ, XDG_RUNTIME_DIR=str(run), AQ_BACKEND='wayland', WAYLAND_DISPLAY=parent_display, HYPRLAND_NO_SD_NOTIFY='1', HYPRLAND_NO_SD_VARS='1')
  env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
  log = (args.artifacts / 'compositor.log').open('w')
  compositor = subprocess.Popen(['Hyprland', '--config', str(config)], env=env, stdout=log, stderr=log)
  clients = []

  def ctl(*argv):
    signature = env.get('HYPRLAND_INSTANCE_SIGNATURE')
    if argv[0] in ('plugin', 'eval') and (not signature or signature == parent_signature or env['XDG_RUNTIME_DIR'] != str(run)):
      raise RuntimeError('Refusing to send a plugin command outside the nested compositor')
    result = subprocess.run(['hyprctl', *argv], env=env, capture_output=True, text=True, timeout=5, check=True)
    if result.stdout.startswith('Err') or result.stdout.startswith('error') or "Couldn't" in result.stdout or 'could not be loaded' in result.stdout:
      raise RuntimeError(result.stdout)
    return result.stdout

  def assign(mapping):
    body = ', '.join(f'[{k}] = "{v}"' for k, v in mapping.items())
    ctl('eval', f'hl.plugin.workspace_backgrounds.set({{ {body} }})')

  def go(workspace):
    ctl('dispatch', f'hl.dsp.focus({{ workspace = {workspace} }})')

  def swipe(call):
    ctl('eval', f'hl.plugin.swipe_driver.{call}')

  def monitor():
    return json.loads(ctl('monitors', '-j'))[0]

  class Frame:
    def __init__(self, name):
      path = args.artifacts / f'{name}.ppm'
      subprocess.run(['grim', '-t', 'ppm', '-o', monitor()['name'], str(path)], env=env, check=True)
      data = path.read_bytes()
      fields, offset = [], 0
      while len(fields) < 4:
        while data[offset:offset + 1].isspace():
          offset += 1
        end = offset
        while not data[end:end + 1].isspace():
          end += 1
        fields.append(data[offset:end])
        offset = end
      self.width, self.height = int(fields[1]), int(fields[2])
      self.pixels = data[offset + 1:]

    def at(self, x, y):
      i = (y * self.width + x) * 3
      return tuple(self.pixels[i:i + 3])

    def row(self, y):
      return [self.at(x, y) for x in range(self.width)]

  def near(a, b, tolerance=6):
    return all(abs(p - q) <= tolerance for p, q in zip(a, b))

  def runs(row):
    # [(color, start, end)] for each stretch of one known color.
    named = {**{f'ws{n}': c for n, c in COLORS.items()}, 'base': BASE, 'window': WINDOW}
    out = []
    for x, pixel in enumerate(row):
      name = next((k for k, c in named.items() if near(pixel, c)), '?')
      if out and out[-1][0] == name:
        out[-1][2] = x
      else:
        out.append([name, x, x])
    return [r for r in out if r[0] != '?']

  def kinds(row):
    return [r[0] for r in runs(row)]

  def edge(row, left, right):
    # x where the `left` stretch ends and `right` begins.
    stretches = runs(row)
    for a, b in zip(stretches, stretches[1:]):
      if a[0] == left and b[0] == right:
        return b[1]
    raise AssertionError(f'no {left}|{right} edge in {[(r[0], r[1], r[2]) for r in stretches]}')

  def settle():
    time.sleep(SLIDE_SECONDS + 0.4)

  try:
    for _ in range(100):
      if compositor.poll() is not None:
        raise RuntimeError('Isolated compositor exited; inspect compositor.log')
      try:
        instances = json.loads(ctl('instances', '-j'))
      except (ValueError, subprocess.SubprocessError):
        instances = []
      if instances:
        env['HYPRLAND_INSTANCE_SIGNATURE'] = instances[0]['instance']
        env['WAYLAND_DISPLAY'] = instances[0]['wl_socket']
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Isolated compositor did not start')
    for _ in range(300):
      if json.loads(ctl('monitors', '-j')):
        break
      time.sleep(.1)
    else:
      raise RuntimeError(f"No nested monitor: {ctl('monitors', 'all', '-j')}")
    time.sleep(.3)
    scale = monitor()['scale']
    gap = round(GAPS_OUT * scale)

    for _ in range(3):
      ctl('plugin', 'load', str(args.library))
      assign({1: images[1], 2: images[2]})
      ctl('reload')
      time.sleep(.2)
      assert compositor.poll() is None
      ctl('plugin', 'unload', str(args.library))
      time.sleep(.1)
    print('PASS: repeated load/assign/reload/unload', flush=True)

    ctl('plugin', 'load', str(args.library))
    ctl('plugin', 'load', str(scratch / 'swipe.so'))
    assign({1: images[1], 2: images[2], 3: images[3]})
    time.sleep(1.5)
    frame = Frame('ws1-assigned')
    assert kinds(frame.row(frame.height // 2)) == ['ws1'], runs(frame.row(frame.height // 2))
    ctl('reload')
    time.sleep(.3)
    frame = Frame('ws1-after-reload')
    assert kinds(frame.row(frame.height // 2)) == ['ws1'], 'assignments must survive a config reload'
    print('PASS: an assigned workspace shows its own background, across a config reload', flush=True)

    # A window on workspace 2, so it outlives being left.
    go(2)
    settle()
    clients.append(subprocess.Popen(['foot', '--config=/dev/null', '--app-id=owb-window', *[a for theme in ('dark', 'light') for key in ('background', 'foreground') for a in ('-o', f'colors-{theme}.{key}=ff00ff')], '-o', 'colors-dark.alpha=1', '-o', 'colors-light.alpha=1',
                                     '-o', 'colors-dark.cursor=ff00ff ff00ff', '-o', 'colors-light.cursor=ff00ff ff00ff', 'sleep', '120'],
                                    env=env, stdout=log, stderr=log))
    for _ in range(50):
      if json.loads(ctl('clients', '-j')):
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Test window did not map')
    time.sleep(1.5)  # its fade in
    frame = Frame('ws2-window')
    middle = frame.row(frame.height // 2)
    assert kinds(middle) == ['ws2', 'window', 'ws2'], runs(middle)

    # Leaving workspace 2 for 1: the background and the window slide together,
    # one gap apart, in every frame.
    go(1)
    edges = []
    for i in range(3):
      time.sleep(SLIDE_SECONDS / 5)
      frame = Frame(f'slide-2-to-1-{i}')
      background = edge(frame.row(gap // 2), 'ws1', 'ws2')
      window = edge(frame.row(frame.height // 2), 'ws2', 'window')
      assert abs(window - background - gap) <= 2, (background, window)
      edges.append(background)
    assert edges[0] < edges[1] < edges[2], edges
    settle()
    print(f'PASS: the background slides with its workspace, window edge {gap}px from it in every frame (edges at {edges})', flush=True)

    # Workspace 1 is empty, so Hyprland destroys it as soon as it is left.
    # Its background has to slide out all the same.
    go(2)
    time.sleep(SLIDE_SECONDS / 2)
    frame = Frame('slide-empty-1-to-2')
    top = frame.row(gap // 2)
    assert kinds(top) == ['ws1', 'ws2'], runs(top)
    middle = frame.row(frame.height // 2)
    assert abs(edge(middle, 'ws2', 'window') - edge(top, 'ws1', 'ws2') - gap) <= 2
    settle()
    print('PASS: an empty workspace that Hyprland destroys on leaving still slides its background out', flush=True)

    # A swipe toward a workspace that does not exist yet, let go short: the
    # new background slides back out with the workspace, never leaving a gap.
    swipe('begin()')
    for step in range(1, 21):
      swipe(f'update({5 * step})')
    swipe('finish()')
    time.sleep(SLIDE_SECONDS / 3)
    frame = Frame('swipe-new-cancelled-sliding-back')
    top = frame.row(gap // 2)
    assert kinds(top) == ['ws2', 'ws3'], runs(top)
    assert abs(edge(frame.row(frame.height // 2), 'window', 'ws2') - (edge(top, 'ws2', 'ws3') - gap)) <= 2
    settle()
    assert json.loads(ctl('activeworkspace', '-j'))['id'] == 2
    print('PASS: a swipe let go short slides the new background back out with its workspace', flush=True)

    # A swipe: the background tracks the fingers, toward a workspace that does
    # not exist yet, and back.
    swipe('begin()')
    swipe('update(100)')
    time.sleep(.2)
    frame = Frame('swipe-third')
    top = frame.row(gap // 2)
    assert kinds(top) == ['ws2', 'ws3'], runs(top)
    first = edge(top, 'ws2', 'ws3')
    middle = frame.row(frame.height // 2)
    assert abs(edge(middle, 'window', 'ws2') - (first - gap)) <= 2
    swipe('update(200)')
    time.sleep(.2)
    frame = Frame('swipe-two-thirds')
    second = edge(frame.row(gap // 2), 'ws2', 'ws3')
    expected = frame.width * (1 - 200 / 300)
    assert abs(second - expected) <= 3 and second < first, (first, second, expected)
    swipe('finish()')
    # Hyprland creates the new workspace already in place; its background must
    # still ride in beside the one leaving.
    time.sleep(SLIDE_SECONDS / 4)
    frame = Frame('swipe-committed-sliding')
    top = frame.row(gap // 2)
    assert kinds(top) == ['ws2', 'ws3'], runs(top)
    assert edge(top, 'ws2', 'ws3') < second
    assert abs(edge(frame.row(frame.height // 2), 'window', 'ws2') - (edge(top, 'ws2', 'ws3') - gap)) <= 2
    settle()
    frame = Frame('swipe-committed')
    assert kinds(frame.row(frame.height // 2)) == ['ws3'], runs(frame.row(frame.height // 2))
    assert json.loads(ctl('activeworkspace', '-j'))['id'] == 3
    print(f'PASS: a swipe carries the backgrounds with the fingers (edge at {first}, then {second}) into a workspace created on release', flush=True)

    # Swipe back from the empty workspace 3 to 2, and let go early: it springs back.
    # Slowly, or Hyprland takes it for a flick and commits it.
    swipe('begin()')
    for step in range(1, 13):
      swipe(f'update({-5 * step})')
    time.sleep(.2)
    frame = Frame('swipe-peek')
    assert kinds(frame.row(gap // 2)) == ['ws2', 'ws3'], runs(frame.row(gap // 2))
    swipe('finish()')
    settle()
    frame = Frame('swipe-cancelled')
    assert kinds(frame.row(frame.height // 2)) == ['ws3']
    print('PASS: a short swipe peeks at the neighbour and settles back', flush=True)

    # In a fade style the background of an empty workspace being left fades
    # out where it is instead of sliding off at full strength.
    ctl('eval', f'hl.animation({{ leaf = "workspaces", enabled = true, speed = {SLIDE_SECONDS * 10}, bezier = "linear", style = "slidefade 50%" }})')
    go(2)
    time.sleep(SLIDE_SECONDS / 2)
    frame = Frame('slidefade-leaving-empty')
    # Workspace 2 arrives from the left, so the right edge shows only the
    # background being left, half faded over the background layer.
    pixel = frame.at(frame.width - frame.width // 20, gap // 2)
    assert all(min(b, c) + 8 < p < max(b, c) - 8 for p, b, c in zip(pixel, BASE, COLORS[3]) if abs(b - c) > 40), pixel
    settle()
    ctl('eval', f'hl.animation({{ leaf = "workspaces", enabled = true, speed = {SLIDE_SECONDS * 10}, bezier = "linear", style = "slide" }})')
    print(f'PASS: in a fade style the background being left fades out in place ({pixel})', flush=True)

    # A workspace without an assignment shows the ordinary background layer.
    go(4)
    settle()
    frame = Frame('unassigned')
    assert kinds(frame.row(frame.height // 2)) == ['base'], runs(frame.row(frame.height // 2))
    print('PASS: a workspace without a background of its own shows the background layer', flush=True)

    # Changing the background of the workspace on screen fades to the new one.
    assign({1: images[1], 2: images[2], 3: images[3], 4: images[4]})
    # Decoding comes first; the one-second fade starts once it is done.
    for _ in range(40):
      frame = Frame('assign-fading')
      middle = frame.at(frame.width // 2, frame.height // 2)
      if not near(middle, BASE):
        break
      time.sleep(.05)
    assert not near(middle, BASE) and not near(middle, COLORS[4]), middle
    time.sleep(1.5)
    frame = Frame('assign-faded')
    assert kinds(frame.row(frame.height // 2)) == ['ws4'], runs(frame.row(frame.height // 2))
    assign({1: images[1], 2: images[2], 3: images[3], 4: images[1]})
    time.sleep(1.5)
    frame = Frame('reassigned')
    assert kinds(frame.row(frame.height // 2)) == ['ws1']
    print('PASS: a new background fades in over the old one on the workspace on screen', flush=True)

    # A picture of another shape covers the screen and stays inside its workspace.
    assign({1: wide, 2: images[2]})
    go(2)
    settle()
    go(1)
    time.sleep(SLIDE_SECONDS / 2)
    frame = Frame('wide-mid-slide')
    top = frame.row(gap // 2)
    assert kinds(top) == ['ws1', 'ws2'], runs(top)
    settle()
    frame = Frame('wide-settled')
    assert kinds(frame.row(gap // 2)) == ['ws1'], runs(frame.row(gap // 2))
    print('PASS: a wider picture is cropped to cover its workspace and never spills into the next', flush=True)

    # Theme switches can put a different picture at the same path.
    same = scratch / 'same.png'
    shutil.copy(images[2], same)
    assign({1: same})
    time.sleep(1.5)
    frame = Frame('same-path-before')
    assert kinds(frame.row(frame.height // 2)) == ['ws2'], runs(frame.row(frame.height // 2))
    shutil.copy(images[3], same)
    assign({1: same})
    time.sleep(1.5)
    frame = Frame('same-path-after')
    assert kinds(frame.row(frame.height // 2)) == ['ws3'], runs(frame.row(frame.height // 2))
    listed = json.loads(ctl('workspacebackgrounds', '-j'))
    assert listed == [{'workspace': 1, 'path': str(same), 'ready': True, 'failed': False}], listed
    print('PASS: a new picture at the same path replaces the old one, and hyprctl lists what each workspace shows', flush=True)

    # A file that is not an image leaves the background layer, and no crash.
    broken = scratch / 'broken.png'
    broken.write_text('not an image')
    assign({1: broken, 2: images[2]})
    time.sleep(1)
    assert compositor.poll() is None
    frame = Frame('broken')
    assert kinds(frame.row(frame.height // 2)) == ['ws3'], 'the last good picture stays until a new one is ready'
    print('PASS: an unreadable picture keeps the last good one and does not crash', flush=True)

    assign({})
    time.sleep(.3)
    frame = Frame('cleared')
    assert kinds(frame.row(frame.height // 2)) == ['base']
    assign({1: images[1], 2: images[2]})
    ctl('plugin', 'unload', str(args.library))
    time.sleep(.3)
    frame = Frame('unloaded')
    assert kinds(frame.row(frame.height // 2)) == ['base']
    assert compositor.poll() is None
    print('PASS: clearing or unloading hands every workspace back to the background layer', flush=True)
  finally:
    for client in clients:
      client.terminate()
    compositor.terminate()
    try:
      compositor.wait(timeout=5)
    except subprocess.TimeoutExpired:
      compositor.kill()
    log.close()
