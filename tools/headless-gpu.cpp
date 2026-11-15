// Test-only LD_PRELOAD shim: gives Aquamarine's headless backend a GPU render
// node, so a fully headless Hyprland (no seat, no parent compositor, no window
// anywhere) can render. Aquamarine's headless backend reports no DRM fd, which
// leaves it without an allocator; hyprtester gets around that by running in a
// VM with a seat. A render node needs no seat and cannot touch any display.
#include <fcntl.h>
#include <cstdlib>
namespace Aquamarine {
  struct CHeadlessBackend {
    int drmFD();
    int drmRenderNodeFD();
  };
  static int node() {
    static int fd = -1;
    if (fd < 0) {
      const char* path = getenv("OWB_RENDER_NODE");
      fd = open(path ? path : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    return fd;
  }
  int CHeadlessBackend::drmFD() { return node(); }
  int CHeadlessBackend::drmRenderNodeFD() { return node(); }
}
