"""A disposable, fully headless Hyprland for tests.

Nothing here can reach a running desktop session: the compositor gets its own
runtime directory (so its own sockets), its own cache directory, no
WAYLAND_DISPLAY (so no parent compositor and no window anywhere), no seat
(LIBSEAT_BACKEND=none, so it never touches a display or input device), and no
D-Bus. Outputs are Hyprland's headless outputs, rendered on the GPU through a
render node, which needs no seat. Every hyprctl call carries the test
instance's own socket path, and refuses to run with any other.

It is the headless mode hyprtester uses; hyprtester itself gets its GPU from a
VM with a seat, which a desktop that is in use cannot give up, so the render
node is handed to Aquamarine's headless backend by a small preload shim
(headless-gpu.cpp) instead.
"""
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

TOOLS = Path(__file__).resolve().parent


def pkg_flags(*names):
  return subprocess.check_output(['pkg-config', '--cflags', *names], text=True).split()


def build(out_dir):
  """Builds the test-only libraries: the GPU shim and the probe plugin."""
  out_dir = Path(out_dir)
  shim = out_dir / 'headless-gpu.so'
  probe = out_dir / 'probe.so'
  subprocess.run(['g++', '-shared', '-fPIC', '-O1', str(TOOLS / 'headless-gpu.cpp'), '-o', str(shim)], check=True)
  subprocess.run(['g++', '-std=c++23', '-shared', '-fPIC', '-fno-gnu-unique', '-O1', str(TOOLS / 'probe.cpp'), '-o', str(probe),
                  *pkg_flags('hyprland', 'pixman-1', 'libdrm', 'lua'), '-lGLESv2'], check=True)
  return shim, probe


class Hyprland:
  def __init__(self, config, workdir, shim, log_name='compositor.log', memory_max='6G', env=None):
    self.workdir = Path(workdir)
    self.workdir.mkdir(parents=True, exist_ok=True)
    # The socket path must fit in sun_path, so the runtime directory stays short.
    self.run = Path(tempfile.mkdtemp(prefix='owb-', dir='/tmp'))
    os.chmod(self.run, 0o700)
    self.cache = self.workdir / 'cache'
    self.cache.mkdir(exist_ok=True)
    # A home of its own too: anything a config or module writes (state files,
    # caches) lands here, never in the real one.
    self.home = self.workdir / 'home'
    for sub in ('', '.config', '.local/state', '.local/share'):
      (self.home / sub).mkdir(parents=True, exist_ok=True)
    self.config = self.workdir / 'hyprland.lua'
    self.config.write_text(config)
    real = os.environ.get('XDG_RUNTIME_DIR', '')
    assert str(self.run) != real
    self.env = {
      'HOME': str(self.home),
      'XDG_CONFIG_HOME': str(self.home / '.config'),
      'XDG_STATE_HOME': str(self.home / '.local/state'),
      'XDG_DATA_HOME': str(self.home / '.local/share'),
      'PATH': '/usr/bin',
      'XDG_RUNTIME_DIR': str(self.run),
      'XDG_CACHE_HOME': str(self.cache),
      'LIBSEAT_BACKEND': 'none',
      'LD_PRELOAD': str(shim),
      'HYPRLAND_NO_SD_NOTIFY': '1',
      'HYPRLAND_NO_SD_VARS': '1',
      'HYPRLAND_NO_CRASHREPORTER': '1',
      'LANG': 'C.UTF-8',
      **(env or {}),
    }
    self.log = (self.workdir / log_name).open('w')
    # Capped like any heavy job on this machine; the compositor is the scope's
    # own process, so its pid is the one we were handed.
    # systemd-run itself needs the session's user manager; the compositor
    # gets only its own environment, through env -i.
    self.proc = subprocess.Popen(['systemd-run', '--user', '--scope', '-q', '-p', f'MemoryMax={memory_max}', '-p', 'MemorySwapMax=0',
                                  'prlimit', f'--core={"unlimited" if os.environ.get("OWB_CORES") else 0}', 'env', '-i', *[f'{k}={v}' for k, v in self.env.items()], 'Hyprland', '--config', str(self.config)],
                                 stdout=self.log, stderr=self.log)
    self.clients = []
    self.signature = None
    for _ in range(200):
      if self.proc.poll() is not None:
        raise RuntimeError(f'headless Hyprland exited; see {self.workdir / log_name}')
      hypr = self.run / 'hypr'
      if hypr.is_dir():
        for d in hypr.iterdir():
          if (d / '.socket.sock').exists():
            self.signature = d.name
      if self.signature:
        try:
          self.ctl('version')
          break
        except Exception:
          pass
      time.sleep(.05)
    else:
      raise RuntimeError('headless Hyprland did not start')
    self.ctl_env = {'PATH': '/usr/bin', 'XDG_RUNTIME_DIR': str(self.run), 'HYPRLAND_INSTANCE_SIGNATURE': self.signature}

  @property
  def pid(self):
    return self.proc.pid

  def ctl(self, *argv, timeout=10):
    env = {'PATH': '/usr/bin', 'XDG_RUNTIME_DIR': str(self.run), 'HYPRLAND_INSTANCE_SIGNATURE': self.signature or 'none'}
    assert env['XDG_RUNTIME_DIR'] == str(self.run) and env['XDG_RUNTIME_DIR'] != os.environ.get('XDG_RUNTIME_DIR')
    result = subprocess.run(['hyprctl', *argv], env=env, capture_output=True, text=True, timeout=timeout)
    out = result.stdout
    if result.returncode != 0 or out.startswith('Err') or out.startswith('error') or "Couldn't" in out or 'could not be loaded' in out:
      raise RuntimeError(f'hyprctl {argv[:2]}: {out}{result.stderr}')
    return out

  def eval(self, lua):
    return self.ctl('eval', lua)

  def json(self, *argv):
    return json.loads(self.ctl(*argv, '-j'))

  def alive(self):
    return self.proc.poll() is None

  def rss_kb(self):
    for line in Path(f'/proc/{self.pid}/status').read_text().splitlines():
      if line.startswith('VmRSS:'):
        return int(line.split()[1])
    return 0

  def wayland_display(self):
    for p in self.run.iterdir():
      if p.name.startswith('wayland-') and not p.name.endswith('.lock'):
        return p.name
    raise RuntimeError('no wayland socket')

  def spawn_window(self, app_id, color='ff00ff', alpha=1.0, blur=False):
    args = ['foot', '--config=/dev/null', f'--app-id={app_id}']
    for theme in ('dark', 'light'):
      for key in ('background', 'foreground'):
        args += ['-o', f'colors-{theme}.{key}={color}']
      args += ['-o', f'colors-{theme}.alpha={alpha}', '-o', f'colors-{theme}.cursor={color} {color}']
      # foot asks for blur itself (ext-background-effect), and only when told to.
      args += ['-o', f'colors-{theme}.blur={"yes" if blur else "no"}']
    args += ['sleep', '600']
    env = dict(self.env, WAYLAND_DISPLAY=self.wayland_display())
    env.pop('LD_PRELOAD')
    before = len(self.json('clients'))
    client = subprocess.Popen(args, env=env, stdout=self.log, stderr=self.log)
    self.clients.append(client)
    for _ in range(100):
      if len(self.json('clients')) > before:
        return client
      time.sleep(.05)
    raise RuntimeError('test window did not map')

  def screenshot(self, path, output):
    env = dict(self.env, WAYLAND_DISPLAY=self.wayland_display())
    env.pop('LD_PRELOAD')
    subprocess.run(['grim', '-t', 'ppm', '-o', output, str(path)], env=env, check=True)
    return Image(path)

  def stop(self):
    for client in self.clients:
      client.terminate()
    for client in self.clients:
      try:
        client.wait(timeout=3)
      except subprocess.TimeoutExpired:
        client.kill()
    self.proc.terminate()
    try:
      self.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
      self.proc.kill()
      self.proc.wait()
    self.log.close()
    for log in self.run.glob('hypr/*/hyprland.log'):
      shutil.copy(log, self.workdir / 'hyprland.log')
    shutil.rmtree(self.run, ignore_errors=True)


class Image:
  def __init__(self, path):
    data = Path(path).read_bytes()
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

  def column(self, x):
    return [self.at(x, y) for y in range(self.height)]


def read_frames(path):
  """The frames the probe recorded: each with its rows decoded to RGB tuples."""
  frames = []
  for line in Path(path).read_text().splitlines():
    if not line.strip():
      continue
    f = json.loads(line)
    rows = {}
    for y, b in f['rows'].items():
      raw = base64.b64decode(b)
      rows[int(y)] = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    f['rows'] = rows
    frames.append(f)
  return frames
