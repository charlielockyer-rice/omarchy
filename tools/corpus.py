"""Feeds workspace-backgrounds-prepare damaged, absurd and hostile files.

Every case runs the real helper as the plugin runs it and checks that it either
prepares a correct picture or refuses with a message, within its time limit,
leaving no output behind, and how much memory it took. Pictures that are fine
must come out exactly cover-cropped.

  python3 tools/corpus.py build/workspace-backgrounds-prepare
"""
import argparse
import os
from pathlib import Path
import resource
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('helper', type=Path)
parser.add_argument('--keep', type=Path, help='keep the corpus in this directory')
args = parser.parse_args()
helper = args.helper.resolve()


def stamp(path):
  st = os.stat(path)
  return f'{st.st_dev}:{st.st_ino}:{st.st_mtime_ns // 10**9}.{st.st_mtime_ns % 10**9:09d}:{st.st_size}'


def png_chunk(kind, data):
  return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


def png_header(w, h):
  return b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))


def magick(*a):
  subprocess.run(['magick', *a], check=True, capture_output=True)


def run(src, w, h, out, extra=()):
  """Runs the helper the way the plugin does; returns (exit, message, seconds, peak MB)."""
  start = time.monotonic()
  try:
    s = stamp(src)
  except OSError:
    s = 'missing'
  proc = subprocess.Popen([*extra, str(helper), str(src), s, str(w), str(h), str(out)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
  _, status, usage = os.wait4(proc.pid, 0)
  message = proc.stdout.read().decode(errors='replace').strip()
  proc.stdout.close()
  code = os.waitstatus_to_exitcode(status)
  return code, message, time.monotonic() - start, usage.ru_maxrss / 1024


with tempfile.TemporaryDirectory(prefix='owb-corpus-', dir=Path.home() / 'scratch') as tmp:
  d = Path(args.keep) if args.keep else Path(tmp)
  d.mkdir(parents=True, exist_ok=True)
  cases = []  # (name, path, should_succeed, note)

  def case(name, ok, note=''):
    cases.append((name, d / name, ok, note))
    return d / name

  # Good pictures, in every format the helper takes.
  magick('-size', '3000x1000', 'xc:#c0392b', str(case('wide.png', True)))
  magick('-size', '1000x3000', 'xc:#2471a3', str(case('tall.jpg', True)))
  magick('-size', '1920x1080', 'xc:#1e8449', str(case('plain.webp', True)))
  magick('-size', '1920x1080', 'xc:#d4ac0d', str(case('plain.bmp', True)))
  magick('-size', '1920x1080', 'xc:#8e44ad', str(case('plain.jxl', True)))
  # Arch's hyprgraphics is built without HEIF, so AVIF may be refused; it must
  # be refused cleanly.
  magick('-size', '1920x1080', 'xc:#e67e22', str(case('plain.avif', None, 'needs hyprgraphics with HEIF')))
  magick('-size', '64x64', 'xc:#00000000', str(case('transparent.png', True, 'comes out opaque black')))
  # Big but within limits: 81 megapixels from a tiny file.
  magick('-size', '9000x9000', 'xc:#336699', str(case('81mp.png', True, 'decoded within the memory limit')))

  # Header claims the helper refuses before decoding.
  case('huge-header.png', False).write_bytes(png_header(100000, 100000) + png_chunk(b'IDAT', zlib.compress(b'\0' * 1000)) + png_chunk(b'IEND', b''))
  case('400mp-header.png', False).write_bytes(png_header(20000, 20000) + png_chunk(b'IDAT', zlib.compress(b'\0' * 100000)) + png_chunk(b'IEND', b''))
  jpg = (d / 'tall.jpg').read_bytes()
  sof = max(jpg.find(b'\xff\xc0'), jpg.find(b'\xff\xc2'))
  case('huge-sof.jpg', False).write_bytes(jpg[:sof + 5] + struct.pack('>HH', 65000, 65000) + jpg[sof + 9:])
  case('huge-vp8x.webp', False).write_bytes(b'RIFF' + struct.pack('<I', 30) + b'WEBPVP8X' + struct.pack('<I', 10) + b'\0' * 4 + (16383).to_bytes(3, 'little') * 2 + b'\0' * 8)
  case('huge.bmp', False).write_bytes(b'BM' + b'\0' * 16 + struct.pack('<ii', 60000, 60000) + b'\0' * 64)

  # Damaged files.
  case('truncated.png', False).write_bytes((d / 'wide.png').read_bytes()[:60])
  good = (d / 'wide.png').read_bytes()
  idat = good.find(b'IDAT')
  case('corrupt-idat.png', False).write_bytes(good[:idat + 8] + bytes(range(256)) * 4 + good[idat + 8 + 1024:])
  case('header-only.jpg', None, 'libjpeg may fill in grey').write_bytes(jpg[:sof + 40])
  case('random.png', False).write_bytes(os.urandom(4096))
  case('empty.jpg', False).write_bytes(b'')
  case('text.webp', False).write_text('not a picture')
  case('fake.svg', False).write_text('<svg xmlns="http://www.w3.org/2000/svg" width="100000" height="100000"/>')
  magick('-size', '64x64', 'xc:red', str(case('anim.gif', False, 'GIFs are left to the shell')))
  os.mkfifo(case('fifo.png', False, 'must not hang'))
  case('directory.png', False).mkdir()
  os.symlink('/dev/zero', case('zero.png', False))
  case('gone.png', False)  # never created

  results = []
  failed = False
  out = d / 'out.pix'
  for name, path, ok, note in cases:
    for size in ((1920, 1080), (5120, 2880)):
      out.unlink(missing_ok=True)
      code, message, seconds, peak = run(path, *size, out)
      produced = out.exists()
      expect = 'ok' if ok else 'refused' if ok is False else 'either'
      good_result = (code == 0 and produced) if ok else (code != 0 and not produced) if ok is False else (code == 0) == produced
      if code == 0 and produced:
        data = out.read_bytes()
        w, h, stride, fmt = struct.unpack('<IIII', data[8:24])
        good_result = good_result and data[:8] == b'OWBPIX1\0' and (w, h, stride) == (size[0], size[1], size[0] * 4) and len(data) == 32 + size[0] * size[1] * 4
      good_result = good_result and seconds < 35 and not list(d.glob('out.pix.*.tmp'))
      failed |= not good_result
      results.append((name, f'{size[0]}x{size[1]}', expect, code, round(seconds, 2), round(peak), message.replace(chr(10), ' / ')[:110], 'PASS' if good_result else 'FAIL'))

  # The limits are the helper's own: with less memory than a picture needs,
  # it fails cleanly instead of growing.
  out.unlink(missing_ok=True)
  code, message, seconds, peak = run(d / '81mp.png', 1920, 1080, out, extra=['prlimit', '--data=200000000'])
  limited = code != 0 and not out.exists()
  failed |= not limited
  limited = limited and code > 0  # a message and an exit code, not a signal
  results.append(('81mp.png under a 200 MB data limit', '1920x1080', 'refused', code, round(seconds, 2), round(peak), message.replace(chr(10), ' / ')[:110], 'PASS' if limited else 'FAIL'))

  # A good picture is cover-cropped exactly: a 3:1 strip, red, onto 16:9 is all red.
  out.unlink(missing_ok=True)
  run(d / 'wide.png', 1920, 1080, out)
  px = out.read_bytes()[32:]
  corners = {px[i * 4:i * 4 + 3] for i in (0, 1919, 1080 * 1920 - 1, 540 * 1920 + 960)}
  cover = corners == {bytes((0x2b, 0x39, 0xc0))}
  failed |= not cover
  results.append(('wide.png covers 16:9 edge to edge', '1920x1080', 'ok', 0, 0, 0, str(corners), 'PASS' if cover else 'FAIL'))

  width = max(len(r[0]) for r in results)
  for r in results:
    print(f'{r[7]}  {r[0]:<{width}}  {r[1]:>9}  expect {r[2]:<7} exit {r[3]:<3} {r[4]:>6}s {r[5]:>5} MB  {r[6]}')
  print('corpus:', 'all passed' if not failed else 'FAILURES', f'({len(results)} runs)')
  sys.exit(1 if failed else 0)
