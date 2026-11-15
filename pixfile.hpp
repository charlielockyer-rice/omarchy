// The contract between the plugin and its image helper: where a prepared
// picture lives in the cache, and the layout of the file.
//
// A prepared picture is a header followed by `height` rows of `width` 32-bit
// XRGB8888 pixels (little endian B, G, R, unused), with no padding, already
// cropped to cover one monitor exactly. The plugin reads nothing else from
// disk, so no image decoder ever runs inside the compositor.
#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace Pix {

  inline constexpr char     MAGIC[8]   = {'O', 'W', 'B', 'P', 'I', 'X', '1', '\0'};
  inline constexpr uint32_t MAX_SIDE   = 16384; // larger than any monitor Hyprland drives
  inline constexpr uint32_t FORMAT     = 0x34325258; // DRM_FORMAT_XRGB8888 ('XR24')

  struct SHeader {
    char     magic[8] = {};
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t stride   = 0;
    uint32_t format   = 0;
    uint64_t source   = 0; // stampHash() of the picture it was made from
  };
  static_assert(sizeof(SHeader) == 32);

  // The picture as it is now: a theme switch can put a different picture at
  // the same path, which must not be mistaken for the one prepared.
  inline std::string stampOf(const struct stat& st) {
    return std::format("{}:{}:{}.{:09}:{}", st.st_dev, st.st_ino, st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_size);
  }

  // FNV-1a, 64 bit: stable across builds, which std::hash is not.
  inline uint64_t hash(const std::string& path, const std::string& stamp) {
    uint64_t h      = 0xcbf29ce484222325ULL;
    auto     absorb = [&](const std::string& s) {
      for (const unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
      }
      h ^= 0xff;
      h *= 0x100000001b3ULL;
    };
    absorb(path);
    absorb(stamp);
    return h;
  }

  inline std::string fileName(uint64_t source, uint32_t width, uint32_t height) {
    return std::format("{:016x}-{}x{}.pix", source, width, height);
  }

  inline uint64_t fileSize(uint32_t width, uint32_t height) {
    return sizeof(SHeader) + (uint64_t)width * height * 4;
  }

  inline bool sane(const SHeader& h, uint64_t source, uint32_t width, uint32_t height) {
    return std::memcmp(h.magic, MAGIC, sizeof(MAGIC)) == 0 && h.width == width && h.height == height && h.stride == width * 4 && h.format == FORMAT && h.source == source &&
        width > 0 && height > 0 && width <= MAX_SIDE && height <= MAX_SIDE;
  }
}
