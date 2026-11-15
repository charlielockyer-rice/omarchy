// workspace-backgrounds-prepare: turns one picture into the pixels the plugin
// draws, in a process of its own.
//
//   workspace-backgrounds-prepare <picture> <stamp> <width> <height> <output>
//
// Decodes <picture>, scales it to cover <width>x<height> and crops the middle
// (macOS's "Fill Screen": never stretched, never letterboxed), and writes the
// result to <output> as described in pixfile.hpp, atomically. <stamp> is the
// picture's identity as the plugin saw it; a picture that has changed since is
// refused (exit 3) rather than prepared under the wrong name.
//
// Decoders are large and parse whatever file they are given, so they never run
// inside the compositor. This process caps its own memory, CPU time, wall time
// and output size before it reads a byte, refuses dimensions it would not
// survive from the file's header before decoding, and leaves no core dump. A
// picture that still breaks a decoder takes down only this process; the plugin
// then keeps the picture the workspace had.

#include "pixfile.hpp"

#include <hyprgraphics/image/Image.hpp>
#include <jxl/decode.h>

#include <cairo/cairo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

  // A 100 megapixel picture decodes to 400 MB; that and the result fit well
  // inside the data limit, and nothing bigger is a sensible wallpaper.
  constexpr uint64_t MAX_PIXELS     = 100'000'000;
  constexpr uint32_t MAX_SOURCE     = 32768;
  constexpr off_t    MAX_FILE_BYTES = 256LL << 20;
  constexpr rlim_t   DATA_LIMIT     = 1024ULL << 20;
  constexpr rlim_t   AS_LIMIT       = 4096ULL << 20;
  constexpr rlim_t   CPU_SECONDS    = 20;
  constexpr unsigned WALL_SECONDS   = 30;

  enum eExit : int {
    EXIT_OK        = 0,
    EXIT_BAD_ARGS  = 2,
    EXIT_CHANGED   = 3,
    EXIT_REFUSED   = 4, // unsupported, absurd or unreadable picture
    EXIT_DECODE    = 5,
    EXIT_WRITE     = 6,
  };

  [[noreturn]] void fail(int code, const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(code);
  }

  void limit(int resource, rlim_t soft, rlim_t hard) {
    const struct rlimit r = {soft, hard};
    setrlimit(resource, &r);
  }

  struct SDims {
    uint64_t    w = 0, h = 0;
    const char* format = "";
  };

  uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
  }
  uint16_t be16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
  }
  uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
  }
  uint32_t le24(const uint8_t* p) {
    return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
  }

  // Walks JPEG segments to the frame header, reading only segment headers, so
  // a large EXIF block in front of it costs nothing.
  std::optional<SDims> jpegDims(int fd, off_t size) {
    off_t   at = 2;
    uint8_t m[9];
    for (int segments = 0; segments < 4096 && at + 4 <= size; ++segments) {
      if (pread(fd, m, 4, at) != 4 || m[0] != 0xFF)
        return std::nullopt;
      const uint8_t marker = m[1];
      if (marker == 0xFF) { // fill byte
        at += 1;
        continue;
      }
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        at += 2;
        continue;
      }
      const uint16_t length = be16(m + 2);
      if (length < 2)
        return std::nullopt;
      const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
      if (frame) {
        if (pread(fd, m, 9, at) != 9)
          return std::nullopt;
        return SDims{be16(m + 7), be16(m + 5), "jpeg"};
      }
      if (marker == 0xDA || marker == 0xD9)
        return std::nullopt; // image data before any frame header
      at += 2 + length;
    }
    return std::nullopt;
  }

  // AVIF/HEIF keep each image's size in an 'ispe' box near the start. Every
  // one found is taken, the largest counting, so a grid's tiles cannot hide
  // the size of the whole.
  std::optional<SDims> ispeDims(const std::vector<uint8_t>& head) {
    SDims best{0, 0, "avif"};
    bool  found = false;
    for (size_t i = 4; i + 16 <= head.size(); ++i) {
      if (std::memcmp(&head[i], "ispe", 4) != 0)
        continue;
      const uint32_t w = be32(&head[i + 8]), h = be32(&head[i + 12]);
      best.w = std::max<uint64_t>(best.w, w);
      best.h = std::max<uint64_t>(best.h, h);
      found  = true;
    }
    return found ? std::optional{best} : std::nullopt;
  }

  std::optional<SDims> jxlDims(const std::vector<uint8_t>& head) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec)
      return std::nullopt;
    std::optional<SDims> out;
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO) == JXL_DEC_SUCCESS && JxlDecoderSetInput(dec, head.data(), head.size()) == JXL_DEC_SUCCESS) {
      JxlDecoderCloseInput(dec);
      if (JxlDecoderProcessInput(dec) == JXL_DEC_BASIC_INFO) {
        JxlBasicInfo info;
        if (JxlDecoderGetBasicInfo(dec, &info) == JXL_DEC_SUCCESS)
          out = SDims{info.xsize, info.ysize, "jxl"};
      }
    }
    JxlDecoderDestroy(dec);
    return out;
  }

  // The picture's size from its header, without decoding it. Only formats the
  // header can be read for are prepared at all.
  std::optional<SDims> dimensions(int fd, off_t size) {
    std::vector<uint8_t> head(std::min<off_t>(size, 256 << 10));
    if (pread(fd, head.data(), head.size(), 0) != (ssize_t)head.size())
      return std::nullopt;
    const auto n  = head.size();
    const auto at = [&](size_t i, const char* s) { return n >= i + std::strlen(s) && std::memcmp(&head[i], s, std::strlen(s)) == 0; };

    if (n >= 24 && at(0, "\x89PNG\r\n\x1a\n") && at(12, "IHDR"))
      return SDims{be32(&head[16]), be32(&head[20]), "png"};
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
      return jpegDims(fd, size);
    if (n >= 30 && at(0, "RIFF") && at(8, "WEBP")) {
      if (at(12, "VP8X"))
        return SDims{le24(&head[24]) + 1ULL, le24(&head[27]) + 1ULL, "webp"};
      if (at(12, "VP8L") && head[20] == 0x2F) {
        const uint32_t bits = le32(&head[21]);
        return SDims{(bits & 0x3FFF) + 1ULL, ((bits >> 14) & 0x3FFF) + 1ULL, "webp"};
      }
      if (at(12, "VP8 ") && head[23] == 0x9D && head[24] == 0x01 && head[25] == 0x2A)
        return SDims{(uint64_t)(le32(&head[26]) & 0x3FFF), (uint64_t)((le32(&head[26]) >> 16) & 0x3FFF), "webp"};
      return std::nullopt;
    }
    if (n >= 26 && at(0, "BM")) {
      const int32_t w = (int32_t)le32(&head[18]), h = (int32_t)le32(&head[22]);
      return SDims{(uint64_t)std::abs((int64_t)w), (uint64_t)std::abs((int64_t)h), "bmp"};
    }
    if ((n >= 2 && head[0] == 0xFF && head[1] == 0x0A) || (n >= 12 && at(4, "JXL ")))
      return jxlDims(head);
    if (n >= 12 && at(4, "ftyp") && (at(8, "avif") || at(8, "avis") || at(8, "mif1") || at(8, "heic")))
      return ispeDims(head);
    return std::nullopt;
  }

  void writeAll(int fd, const void* data, size_t len) {
    auto p = (const uint8_t*)data;
    while (len > 0) {
      const ssize_t w = write(fd, p, len);
      if (w < 0 && errno == EINTR)
        continue;
      if (w <= 0)
        fail(EXIT_WRITE, std::string{"cannot write: "} + std::strerror(errno));
      p += w;
      len -= w;
    }
  }
}

int prepare(int argc, char** argv);

int main(int argc, char** argv) {
  // A picture that needs more memory than the limits allow ends here, with a
  // message, rather than as an abort.
  try {
    return prepare(argc, argv);
  } catch (const std::bad_alloc&) { fail(EXIT_DECODE, "the picture needs more memory than the image helper may use"); } catch (const std::exception& e) {
    fail(EXIT_DECODE, std::string{"cannot prepare: "} + e.what());
  }
}

int prepare(int argc, char** argv) {
  // Limits first, before anything has looked at the picture.
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  prctl(PR_SET_DUMPABLE, 0);
  limit(RLIMIT_CORE, 0, 0);
  limit(RLIMIT_DATA, DATA_LIMIT, DATA_LIMIT);
  limit(RLIMIT_AS, AS_LIMIT, AS_LIMIT);
  limit(RLIMIT_CPU, CPU_SECONDS, CPU_SECONDS + 5);
  alarm(WALL_SECONDS);
  setpriority(PRIO_PROCESS, 0, 10);

  if (argc != 6)
    fail(EXIT_BAD_ARGS, "usage: workspace-backgrounds-prepare <picture> <stamp> <width> <height> <output>");

  const std::string path = argv[1], stamp = argv[2], output = argv[5];
  char*             end  = nullptr;
  const long        W = std::strtol(argv[3], &end, 10);
  const bool        wOk = *end == '\0';
  const long        H = std::strtol(argv[4], &end, 10);
  if (!wOk || *end != '\0' || W <= 0 || H <= 0 || W > Pix::MAX_SIDE || H > Pix::MAX_SIDE)
    fail(EXIT_BAD_ARGS, "bad output size");
  const uint64_t outBytes = Pix::fileSize(W, H);
  limit(RLIMIT_FSIZE, outBytes + 4096, outBytes + 4096);

  // Not blocking, so a FIFO or a device named as a picture cannot hang this.
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    fail(EXIT_REFUSED, std::string{"cannot open: "} + std::strerror(errno));
  struct stat st = {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    fail(EXIT_REFUSED, "not a regular file");
  if (Pix::stampOf(st) != stamp)
    fail(EXIT_CHANGED, "the picture changed while it was waiting to be prepared");
  if (st.st_size <= 0)
    fail(EXIT_REFUSED, "the file is empty");
  if (st.st_size > MAX_FILE_BYTES)
    fail(EXIT_REFUSED, std::format("the file is {} MB; pictures over {} MB are not used", st.st_size >> 20, MAX_FILE_BYTES >> 20));

  const auto dims = dimensions(fd, st.st_size);
  if (!dims)
    fail(EXIT_REFUSED, "not a PNG, JPEG, WebP, BMP, JPEG XL or AVIF picture, or its header is damaged");
  if (dims->w == 0 || dims->h == 0 || dims->w > MAX_SOURCE || dims->h > MAX_SOURCE || dims->w * dims->h > MAX_PIXELS)
    fail(EXIT_REFUSED, std::format("{} is {}x{}; pictures over {} megapixels or {} pixels a side are not used", dims->format, dims->w, dims->h, MAX_PIXELS / 1'000'000, MAX_SOURCE));

  // Decoded through our own descriptor, so it is the file just checked.
  Hyprgraphics::CImage image(std::format("/proc/self/fd/{}", fd));
  if (!image.success())
    fail(EXIT_DECODE, "cannot decode: " + image.getError());

  cairo_surface_t* source = image.cairoSurface()->cairo();
  const double     sw = cairo_image_surface_get_width(source), sh = cairo_image_surface_get_height(source);
  if (sw <= 0 || sh <= 0 || std::abs(sw - (double)dims->w) > 1 || std::abs(sh - (double)dims->h) > 1)
    fail(EXIT_DECODE, std::format("decoded {}x{}, but the header said {}x{}", sw, sh, dims->w, dims->h));

  // Cover and crop: the shorter fit wins, the overhang is cut evenly from both
  // sides. Painted over black, so a picture with transparency comes out opaque.
  cairo_surface_t* target = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  if (cairo_surface_status(target) != CAIRO_STATUS_SUCCESS)
    fail(EXIT_DECODE, "out of memory for the result");
  cairo_t*     cr    = cairo_create(target);
  const double scale = std::max(W / sw, H / sh);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);
  cairo_translate(cr, (W - sw * scale) / 2.0, (H - sh * scale) / 2.0);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, source, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(target);
  if (cairo_surface_status(target) != CAIRO_STATUS_SUCCESS)
    fail(EXIT_DECODE, "cannot scale the picture");

  // Changed while decoding: what was decoded may be either version.
  struct stat after = {};
  if (stat(path.c_str(), &after) != 0 || Pix::stampOf(after) != stamp)
    fail(EXIT_CHANGED, "the picture changed while it was being prepared");

  Pix::SHeader header;
  std::memcpy(header.magic, Pix::MAGIC, sizeof(Pix::MAGIC));
  header.width  = W;
  header.height = H;
  header.stride = W * 4;
  header.format = Pix::FORMAT;
  header.source = Pix::hash(path, stamp);

  const std::string temp = std::format("{}.{}.tmp", output, getpid());
  const int         out  = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out < 0)
    fail(EXIT_WRITE, std::string{"cannot create "} + temp + ": " + std::strerror(errno));
  writeAll(out, &header, sizeof(header));
  const uint8_t* pixels = cairo_image_surface_get_data(target);
  const int      stride = cairo_image_surface_get_stride(target);
  for (long y = 0; y < H; ++y)
    writeAll(out, pixels + (size_t)y * stride, (size_t)W * 4);
  if (close(out) != 0 || rename(temp.c_str(), output.c_str()) != 0) {
    unlink(temp.c_str());
    fail(EXIT_WRITE, std::string{"cannot finish "} + output + ": " + std::strerror(errno));
  }

  cairo_surface_destroy(target);
  return EXIT_OK;
}
