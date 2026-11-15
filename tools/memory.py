"""What five workspace backgrounds cost, per monitor resolution.

  python3 tools/memory.py build/workspace-backgrounds.so

For each resolution, a headless Hyprland preloads five different photo-like
pictures (6000x4000 JPEGs) for workspaces 1-5 and reports: texture memory
(the GPU), the compositor's own memory before and after, the disk cache, the
time to have all five ready, the slowest texture upload, and the image
helper's peak memory per picture.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import headless  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('library', type=Path)
parser.add_argument('--work', type=Path, default=Path.home() / '.cache/omarchy-hyprland-workspace-backgrounds-memory')
args = parser.parse_args()
library = args.library.resolve()
helper = library.parent / 'workspace-backgrounds-prepare'
shutil.rmtree(args.work, ignore_errors=True)
args.work.mkdir(parents=True)
shim, probe = headless.build(args.work)

pictures = []
for i in range(5):
  p = args.work / f'photo{i}.jpg'
  subprocess.run(['magick', '-seed', str(i), '-size', '6000x4000', 'plasma:', '-quality', '90', str(p)], check=True)
  pictures.append(p)

rows = []
for name, (w, h) in {'1080p': (1920, 1080), '4K': (3840, 2160), '5K': (5120, 2880)}.items():
  hy = headless.Hyprland(f'''
hl.monitor({{ output = "", mode = "{w}x{h}@60", position = "0x0", scale = 1 }})
hl.config({{ misc = {{ disable_hyprland_logo = true, disable_splash_rendering = true }}, xwayland = {{ enabled = false }}, debug = {{ disable_logs = false }} }})
''', args.work / name, shim)
  try:
    hy.ctl('output', 'create', 'headless', 'M')
    time.sleep(1)
    hy.ctl('plugin', 'load', str(library))
    hy.eval(f'hl.plugin.workspace_backgrounds.configure({{ helper = "{helper}" }})')
    time.sleep(.5)
    before = hy.rss_kb()
    entries = ', '.join(f'{{ workspace = {i + 1}, path = "{p}", monitor = "M", preload = true }}' for i, p in enumerate(pictures))
    start = time.time()
    hy.eval(f'hl.plugin.workspace_backgrounds.set({{ {entries} }})')
    while True:
      st = hy.json('workspacebackgrounds')
      if not st['pending'] and st['helpers'] == 0 and st['loading'] == 0 and st['uploads_waiting'] == 0:
        break
      time.sleep(.05)
    ready = time.time() - start
    time.sleep(1)
    after = hy.rss_kb()
    cache = sum(f.stat().st_size for f in (hy.cache / 'omarchy/workspace-backgrounds').glob('*.pix'))
    # The helper's own peak, run the way the plugin runs it.
    out = args.work / 'peak.pix'
    st0 = os.stat(pictures[0])
    stamp = f'{st0.st_dev}:{st0.st_ino}:{st0.st_mtime_ns // 10**9}.{st0.st_mtime_ns % 10**9:09d}:{st0.st_size}'
    pid = os.posix_spawn(str(helper), [str(helper), str(pictures[0]), stamp, str(w), str(h), str(out)], {})
    _, _, usage = os.wait4(pid, 0)
    rows.append((name, f'{w}x{h}', len(st['textures']), st['bytes'] / 2**20, (after - before) / 1024, cache / 2**20, ready, st['max_upload_ms'], usage.ru_maxrss / 1024))
  finally:
    hy.stop()

print(f'{"":6} {"output":>10} {"tex":>4} {"GPU MiB":>8} {"Hyprland RSS +MiB":>18} {"disk MiB":>9} {"ready s":>8} {"max upload ms":>14} {"helper peak MiB":>16}')
for r in rows:
  print(f'{r[0]:6} {r[1]:>10} {r[2]:>4} {r[3]:>8.1f} {r[4]:>18.1f} {r[5]:>9.1f} {r[6]:>8.2f} {r[7]:>14.1f} {r[8]:>16.0f}')
