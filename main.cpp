// Gives every workspace its own background, drawn by the compositor as part of
// the workspace, so it moves with the workspace's own render offset: the slide
// of a workspace switch, a finger-following swipe, and the fade style alike.
//
// A layer-shell wallpaper sits under every workspace at once and cannot know
// where a workspace is mid-switch, so it can only swap after the fact. Drawing
// here, right after the background layer and before the bottom layer, puts the
// picture exactly where Hyprland puts that workspace's windows, every frame.
//
// No image is decoded in here. workspace-backgrounds-prepare, a process of its
// own with its own memory limits, turns each picture into raw pixels cropped to
// a monitor (pixfile.hpp) and caches them on disk; the plugin only reads those
// files, checks them, and uploads them as textures. A picture that breaks a
// decoder breaks that process, and the workspace keeps the picture it had.

#define WLR_USE_UNSTABLE

#include "pixfile.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/MainLoopExecutor.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
// glibc 2.44 declares these without C linkage; without this they bind to
// C++ names that do not exist.
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

extern char** environ;

namespace {

  HANDLE PHANDLE = nullptr;

  using Clock = std::chrono::steady_clock;

  struct SSize {
    int  w = 0, h = 0;
    auto operator<=>(const SSize&) const = default;
  };

  struct SPicture {
    std::string path;
    std::string stamp; // empty when the file could not be found
    auto        operator<=>(const SPicture&) const = default;
    bool        empty() const {
      return path.empty();
    }
    uint64_t id() const {
      return Pix::hash(path, stamp);
    }
  };

  SPicture pictureAt(const std::string& path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0)
      return {path, ""};
    return {path, Pix::stampOf(st)};
  }

  // One picture prepared for one monitor size. Workspaces that show the same
  // picture on monitors of the same size share one of these, on disk and on
  // the GPU.
  struct SKey {
    SPicture picture;
    SSize    size;
    auto     operator<=>(const SKey&) const = default;
  };

  // How far a texture is shrunk to stay inside the memory budget: each level
  // halves both sides. The picture is still drawn at the monitor's full size.
  enum eLevel : uint8_t {
    LEVEL_FULL    = 0,
    LEVEL_HALF    = 1,
    LEVEL_QUARTER = 2,
  };

  const char* levelName(eLevel level) {
    return level == LEVEL_FULL ? "full" : level == LEVEL_HALF ? "half" : "quarter";
  }

  SSize atLevel(SSize size, eLevel level) {
    for (int i = 0; i < level; ++i)
      size = {(size.w + 1) / 2, (size.h + 1) / 2};
    return size;
  }

  uint64_t bytesOf(SSize size) {
    return (uint64_t)size.w * size.h * 4;
  }

  // What the integration asked for one workspace.
  struct SAssign {
    SPicture    picture;
    std::string monitor; // connector the workspace belongs to, for one that does not exist yet
    bool        preload = false;
    auto        operator<=>(const SAssign&) const = default;
  };
  using SAssignments = std::map<WORKSPACEID, SAssign>;

  // What one workspace draws: its picture, and while a new one fades in, the
  // one it had underneath.
  struct SView {
    SPicture          current;
    SPicture          previous;
    PHLANIMVAR<float> fade;
  };

  struct STexture {
    SP<Render::ITexture> tex;
    eLevel               level = LEVEL_FULL;
    uint64_t             bytes = 0;
  };

  enum eDisk : uint8_t {
    DISK_MISSING,
    DISK_PREPARING,
    DISK_READY,
  };

  // A running workspace-backgrounds-prepare.
  struct SHelper {
    SKey               key;
    int                pidfd  = -1;
    int                output = -1; // its stdout and stderr
    wl_event_source*   source = nullptr;
    std::string        message;
    Clock::time_point  started;
  };

  // Reading a prepared file, on the worker thread. Owned on the main thread
  // only: Hyprland's shared pointers count without atomics, so the worker is
  // handed a plain pointer, and the main thread keeps the job alive until it
  // has finished it.
  struct SLoad {
    SKey                  key;
    eLevel                level = LEVEL_FULL;
    std::string           file;
    SSize                 size; // of the result
    std::vector<uint8_t>  pixels;
    std::string           error;
    SP<CMainLoopExecutor> done;
  };

  struct SNeed {
    SKey key;
    int  priority = 0; // lower first
    bool resident = false;
    eLevel level  = LEVEL_FULL;
  };

  struct SConfig {
    uint64_t    budget    = 384ULL << 20;
    uint64_t    cacheCap  = 1024ULL << 20;
    int         helpers   = 2;
    std::string cacheDir;
    std::string helper;
  };

  // A workspace Hyprland destroyed as it was left (it was empty), whose
  // picture still has to finish leaving. Its slide is copied from the
  // workspace's own the moment the switch starts, same curve, same start, so
  // it moves exactly as the workspace would have, in every switch style, and
  // a switch interrupted by another leaves each on its own path.
  struct SLeaving {
    WORKSPACEID          id = WORKSPACE_INVALID;
    PHLANIMVAR<Vector2D> offset;
    PHLANIMVAR<float>    alpha;
  };

  struct STrack {
    PHLWORKSPACEREF      current;
    std::vector<SLeaving> leaving;
    WORKSPACEID active           = WORKSPACE_INVALID;
    WORKSPACEID swipeLeaving     = WORKSPACE_INVALID; // left by a swipe, which Hyprland does not animate as a switch
    Vector2D    side;
    bool        swiping          = false;
    bool        leavingFromSwipe = false;
    WORKSPACEID swipeTarget      = WORKSPACE_INVALID;
    WORKSPACEID swipeBegin       = WORKSPACE_INVALID;
    Vector2D    swipeSide;
  };

  struct SDrawn {
    Render::ITexture* tex = nullptr;
    CBox              box;
    float             alpha = 1.F;
    bool              operator==(const SDrawn& o) const {
      return tex == o.tex && box == o.box && alpha == o.alpha;
    }
  };

  struct SState {
    SConfig                             config;

    SAssignments                        shown;
    std::optional<SAssignments>         pending;
    std::map<WORKSPACEID, SView>        views;

    std::map<SKey, STexture>            textures;
    std::map<SPicture, std::string>     failed; // per picture: no size can fix it
    std::map<SKey, eDisk>               disk;
    std::map<SKey, UP<SHelper>>         helpers;
    std::map<SKey, SP<SLoad>>           loads;   // reading on the worker
    std::deque<SP<SLoad>>               uploads; // read, waiting for a quiet moment
    std::vector<SP<SLoad>>              retired;
    std::vector<SNeed>                  needs;

    std::thread                         worker;
    std::mutex                          mutex;
    std::condition_variable             wake;
    std::deque<SLoad*>                  queue;
    bool                                exiting = false;

    SP<CEventLoopTimer>                 timer;
    std::map<MONITORID, STrack>         tracks;
    std::map<MONITORID, std::vector<SDrawn>> drawn;
    std::set<MONITORID>                 changed; // background differs from last frame's
    std::map<MONITORID, SSize>          sizes;   // as of the last plan

    // What renderAllClientsForWorkspace is drawing right now, from its hook.
    struct {
      bool         active = false;
      PHLWORKSPACE workspace;
      bool         plain = true; // no translate or scale: the monitor's own frame
    } rendering;
    CFunctionHook*                      renderHook = nullptr;

    // Measured, for hyprctl and the tests.
    double                              lastUploadMs = 0, maxUploadMs = 0;
    uint64_t                            uploadsDone = 0, urgentUploads = 0, uploadsInMotion = 0, commits = 0, discarded = 0, helpersStarted = 0, helpersStopped = 0;

    std::vector<CHyprSignalListener>    listeners;
    SP<SHyprCtlCommand>                 hyprctl;
  };

  UP<SState> g_state;

  void replan();
  void schedule(std::chrono::milliseconds in = std::chrono::milliseconds{0});

  // ---------------------------------------------------------------- monitors

  SSize sizeOf(PHLMONITOR monitor) {
    return {(int)std::round(monitor->m_transformedSize.x), (int)std::round(monitor->m_transformedSize.y)};
  }

  std::vector<PHLMONITOR> monitors() {
    std::vector<PHLMONITOR> out;
    for (const auto& m : State::monitorState()->monitors()) {
      if (m && !m->isMirror() && m->m_transformedSize.x >= 1 && m->m_transformedSize.y >= 1)
        out.push_back(m);
    }
    return out;
  }

  PHLWORKSPACE workspaceOf(WORKSPACEID id) {
    return State::workspaceState()->query().id(id).run();
  }

  // The monitor a workspace is on, or for one that does not exist yet, the one
  // the integration said it belongs to.
  PHLMONITOR monitorFor(WORKSPACEID id, const SAssign& assign) {
    if (const auto ws = workspaceOf(id); ws && ws->m_monitor)
      return ws->m_monitor.lock();
    for (const auto& m : monitors()) {
      if (m->m_name == assign.monitor)
        return m;
    }
    return nullptr;
  }

  bool visible(WORKSPACEID id) {
    for (const auto& m : monitors()) {
      if (m->m_activeWorkspace && m->m_activeWorkspace->m_id == id)
        return true;
    }
    return false;
  }

  bool moving(const PHLWORKSPACE& ws) {
    return ws->m_renderOffset->isBeingAnimated() || ws->m_alpha->isBeingAnimated() || ws->m_forceRendering;
  }

  bool workspacesMoving() {
    if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
      return true;
    for (const auto& workspace : State::workspaceState()->workspaces()) {
      const auto ws = workspace.lock();
      if (ws && !ws->m_isSpecialWorkspace && moving(ws))
        return true;
    }
    return false;
  }

  // Animating anything this plugin draws: no time for a texture upload.
  bool busy() {
    if (workspacesMoving())
      return true;
    return std::ranges::any_of(g_state->views, [](const auto& e) { return e.second.fade && e.second.fade->isBeingAnimated(); });
  }

  void damageAll() {
    for (const auto& m : State::monitorState()->monitors())
      g_pHyprRenderer->damageMonitor(m);
  }

  void markChanged() {
    for (const auto& m : State::monitorState()->monitors())
      g_state->changed.insert(m->m_id);
    damageAll();
  }

  // ------------------------------------------------------------ pixel files

  std::string cacheFile(const SKey& key) {
    return g_state->config.cacheDir + "/" + Pix::fileName(key.picture.id(), key.size.w, key.size.h);
  }

  bool fileComplete(const std::string& path, const SKey& key) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
      return false;
    struct stat  st = {};
    Pix::SHeader header;
    const bool   ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == Pix::fileSize(key.size.w, key.size.h) &&
        pread(fd, &header, sizeof(header), 0) == sizeof(header) && Pix::sane(header, key.picture.id(), key.size.w, key.size.h);
    close(fd);
    return ok;
  }

  bool readFully(int fd, uint8_t* to, size_t len, off_t at) {
    while (len > 0) {
      const ssize_t r = pread(fd, to, len, at);
      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0)
        return false;
      to += r;
      at += r;
      len -= r;
    }
    return true;
  }

  // Reads a prepared file, never trusting it: its size, header and picture
  // must be exactly the ones asked for. Read rather than mapped, so a file cut
  // short underneath cannot take the compositor down with SIGBUS. A smaller
  // level is box-filtered while reading, a few rows at a time.
  void readPixels(SLoad& load) {
    const auto& key = load.key;
    const int   fd  = open(load.file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      load.error = std::format("cannot open {}: {}", load.file, strerror(errno));
      return;
    }

    struct stat  st = {};
    Pix::SHeader header;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != Pix::fileSize(key.size.w, key.size.h) || !readFully(fd, (uint8_t*)&header, sizeof(header), 0) ||
        !Pix::sane(header, key.picture.id(), key.size.w, key.size.h)) {
      load.error = std::format("{} is not the prepared picture it should be", load.file);
      close(fd);
      return;
    }

    const size_t rowBytes = (size_t)key.size.w * 4;
    load.size             = atLevel(key.size, load.level);
    load.pixels.resize(bytesOf(load.size));

    if (load.level == LEVEL_FULL) {
      if (!readFully(fd, load.pixels.data(), load.pixels.size(), sizeof(header)))
        load.error = std::format("cannot read {}", load.file);
      futimens(fd, nullptr); // recently used, for the cache's oldest-first pruning
      close(fd);
      return;
    }

    const int            factor = 1 << load.level;
    std::vector<uint8_t> band(rowBytes * factor);
    for (int oy = 0; oy < load.size.h; ++oy) {
      const int y0   = oy * factor;
      const int rows = std::min(factor, key.size.h - y0);
      if (!readFully(fd, band.data(), rowBytes * rows, sizeof(header) + (off_t)rowBytes * y0)) {
        load.error = std::format("cannot read {}", load.file);
        break;
      }
      uint8_t* out = load.pixels.data() + (size_t)oy * load.size.w * 4;
      for (int ox = 0; ox < load.size.w; ++ox) {
        const int x0   = ox * factor;
        const int cols = std::min(factor, key.size.w - x0);
        uint32_t  sum[3] = {0, 0, 0};
        for (int dy = 0; dy < rows; ++dy) {
          const uint8_t* p = band.data() + (size_t)dy * rowBytes + (size_t)x0 * 4;
          for (int dx = 0; dx < cols; ++dx)
            for (int c = 0; c < 3; ++c)
              sum[c] += p[dx * 4 + c];
        }
        const int n = rows * cols;
        for (int c = 0; c < 3; ++c)
          out[ox * 4 + c] = (uint8_t)((sum[c] + n / 2) / n);
        out[ox * 4 + 3] = 0xFF;
      }
    }
    futimens(fd, nullptr);
    close(fd);
  }

  void workerLoop() {
    while (true) {
      SLoad* load = nullptr;
      {
        std::unique_lock lock(g_state->mutex);
        g_state->wake.wait(lock, [] { return g_state->exiting || !g_state->queue.empty(); });
        if (g_state->exiting)
          return;
        load = g_state->queue.front();
        g_state->queue.pop_front();
      }

      readPixels(*load);
      // The last touch: once signalled, the main thread may finish and free it.
      load->done->signal();
    }
  }

  // ---------------------------------------------------------------- textures

  // Opaque: the picture covers its workspace edge to edge, so the GPU neither
  // reads alpha from it nor blends it over what is below.
  void upload(const SP<SLoad>& load, bool urgent) {
    const auto start = Clock::now();
    auto       tex   = g_pHyprRenderer->createTexture(Pix::FORMAT, load->pixels.data(), load->size.w * 4, Vector2D{(double)load->size.w, (double)load->size.h}, false, true);
    const auto ms    = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    // The GPU has it now; the copy in this process goes at once.
    std::vector<uint8_t>{}.swap(load->pixels);

    g_state->lastUploadMs = ms;
    g_state->maxUploadMs  = std::max(g_state->maxUploadMs, ms);
    g_state->uploadsDone++;
    if (urgent)
      g_state->urgentUploads++;
    if (workspacesMoving())
      g_state->uploadsInMotion++;

    if (!tex || !tex->ok()) {
      Log::logger->log(Log::ERR, "[workspace-backgrounds] cannot upload {}", load->file);
      return;
    }

    g_state->textures[load->key] = {tex, load->level, bytesOf(load->size)};
    Log::logger->log(Log::DEBUG, "[workspace-backgrounds] uploaded {} ({}, {}) in {:.1f} ms{}", load->key.picture.path, levelName(load->level), load->size.w, ms,
                     urgent ? ", urgently" : "");
    markChanged();
  }

  const SNeed* needFor(const SKey& key) {
    for (const auto& n : g_state->needs) {
      if (n.key == key)
        return &n;
    }
    return nullptr;
  }

  // A read finished. Only what is still wanted, at the level still wanted, is
  // kept; anything a newer plan made obsolete is dropped unseen.
  void finishLoad(const SKey& key) {
    if (!g_state)
      return;
    const auto it = g_state->loads.find(key);
    if (it == g_state->loads.end())
      return;

    const auto load = it->second;
    g_state->loads.erase(it);
    g_state->retired.clear();
    g_state->retired.push_back(load);

    const auto need = needFor(key);
    const auto have = g_state->textures.find(key);
    if (!need || !need->resident || need->level != load->level || (have != g_state->textures.end() && have->second.level == load->level)) {
      g_state->discarded++;
      replan();
      return;
    }

    if (!load->error.empty()) {
      // The file went bad on disk: prepare it again.
      Log::logger->log(Log::ERR, "[workspace-backgrounds] {}", load->error);
      g_state->disk[key] = DISK_MISSING;
      unlink(load->file.c_str());
      replan();
      return;
    }

    g_state->uploads.push_back(load);
    schedule();
  }

  void startLoad(const SNeed& need) {
    auto load   = makeShared<SLoad>();
    load->key   = need.key;
    load->level = need.level;
    load->file  = cacheFile(need.key);
    const auto key = need.key;
    load->done  = makeShared<CMainLoopExecutor>([key] { finishLoad(key); });
    g_state->loads[key] = load;

    std::scoped_lock lock(g_state->mutex);
    g_state->queue.push_back(load.get());
    g_state->wake.notify_one();
  }

  // Something about to be on screen without its texture: read and upload it
  // now, on this thread, rather than show a blank and fade it in late. One
  // frame takes longer; the switch itself is not delayed.
  void loadNow(const SKey& key) {
    if (g_state->textures.contains(key))
      return;

    for (auto it = g_state->uploads.begin(); it != g_state->uploads.end(); ++it) {
      if ((*it)->key == key) {
        const auto load = *it;
        g_state->uploads.erase(it);
        upload(load, true);
        return;
      }
    }

    if (g_state->failed.contains(key.picture) || !fileComplete(cacheFile(key), key))
      return;

    const auto need = needFor(key);
    auto       load = makeShared<SLoad>();
    load->key       = key;
    load->level     = need ? need->level : LEVEL_FULL;
    load->file      = cacheFile(key);
    readPixels(*load);
    if (!load->error.empty())
      return;

    // A read of this key still queued for the worker is now redundant; one
    // already running is thrown away when it finishes.
    if (const auto it = g_state->loads.find(key); it != g_state->loads.end()) {
      bool dropped = false;
      {
        std::scoped_lock lock(g_state->mutex);
        dropped = std::erase(g_state->queue, it->second.get()) > 0;
      }
      if (dropped)
        g_state->loads.erase(it);
    }
    upload(load, true);
  }

  // ----------------------------------------------------------------- helpers

  void endHelper(SHelper& helper) {
    if (helper.source)
      wl_event_source_remove(helper.source);
    if (helper.output >= 0)
      close(helper.output);
    if (helper.pidfd >= 0)
      close(helper.pidfd);
    helper.source = nullptr;
    helper.output = helper.pidfd = -1;
  }

  void killHelper(SHelper& helper) {
    if (helper.pidfd >= 0)
      pidfd_send_signal(helper.pidfd, SIGKILL, nullptr, 0);
    endHelper(helper);
  }

  void helperDone(const SKey& key) {
    const auto it = g_state->helpers.find(key);
    if (it == g_state->helpers.end())
      return;
    auto helper = std::move(it->second);
    g_state->helpers.erase(it);
    endHelper(*helper);

    if (fileComplete(cacheFile(key), key)) {
      g_state->disk[key] = DISK_READY;
    } else {
      auto message = helper->message;
      while (!message.empty() && (message.back() == '\n' || message.back() == ' '))
        message.pop_back();
      if (message.empty())
        message = "the image helper stopped without a result (it crashed or ran out of memory)";
      Log::logger->log(Log::ERR, "[workspace-backgrounds] cannot load {}: {}", key.picture.path, message);
      g_state->disk[key]              = DISK_MISSING;
      g_state->failed[key.picture]    = message;
    }
    replan();
  }

  int onHelperOutput(int fd, uint32_t mask, void* data) {
    auto*   helper = (SHelper*)data;
    char    buf[1024];
    ssize_t r = read(fd, buf, sizeof(buf));
    if (r > 0) {
      if (helper->message.size() < 4096)
        helper->message.append(buf, r);
      return 0;
    }
    if (r < 0 && (errno == EINTR || errno == EAGAIN))
      return 0;
    // End of output: the helper has exited, one way or another.
    const auto key = helper->key;
    helperDone(key);
    return 0;
  }

  std::string defaultHelper() {
    Dl_info info = {};
    if (dladdr((void*)&defaultHelper, &info) && info.dli_fname)
      return (std::filesystem::path{info.dli_fname}.parent_path() / "workspace-backgrounds-prepare").string();
    return "/usr/lib/omarchy-hyprland-workspace-backgrounds/workspace-backgrounds-prepare";
  }

  std::string defaultCacheDir() {
    const char* xdg  = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    const auto  base = xdg && *xdg ? std::string{xdg} : std::string{home ? home : "/tmp"} + "/.cache";
    return base + "/omarchy/workspace-backgrounds";
  }

  bool startHelper(const SKey& key) {
    std::error_code ec;
    std::filesystem::create_directories(g_state->config.cacheDir, ec);

    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) != 0)
      return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 2);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    const auto  width = std::to_string(key.size.w), height = std::to_string(key.size.h), out = cacheFile(key);
    const char* argv[] = {g_state->config.helper.c_str(), key.picture.path.c_str(), key.picture.stamp.c_str(), width.c_str(), height.c_str(), out.c_str(), nullptr};

    int        pidfd = -1;
    const int  err   = pidfd_spawn(&pidfd, g_state->config.helper.c_str(), &actions, &attr, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipeFds[1]);

    if (err != 0) {
      close(pipeFds[0]);
      const auto message = std::format("cannot run the image helper {}: {}", g_state->config.helper, strerror(err));
      Log::logger->log(Log::ERR, "[workspace-backgrounds] {}", message);
      g_state->failed[key.picture] = message;
      return false;
    }

    auto helper     = makeUnique<SHelper>();
    helper->key     = key;
    helper->pidfd   = pidfd;
    helper->output  = pipeFds[0];
    helper->started = Clock::now();
    helper->source  = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, pipeFds[0], WL_EVENT_READABLE, onHelperOutput, helper.get());
    g_state->disk[key] = DISK_PREPARING;
    g_state->helpers[key] = std::move(helper);
    g_state->helpersStarted++;
    return true;
  }

  // -------------------------------------------------------------------- plan

  // Everything wanted right now, most urgent first: the pictures of the
  // workspaces on screen, then the ones that could be switched to without
  // waiting (the preloaded slots and every workspace that exists), then
  // pictures only kept on disk, for workspaces that would have to be created.
  //
  // While a new set is being prepared, the set on screen keeps what its
  // existing workspaces show, outside the budget: that overlap lasts only
  // until the swap.
  std::vector<SNeed> computeNeeds() {
    std::map<SKey, SNeed> merged;

    const auto add = [&](const SAssignments& set, bool target) {
      for (const auto& [id, assign] : set) {
        if (assign.picture.empty() || assign.picture.stamp.empty() || g_state->failed.contains(assign.picture))
          continue;
        const auto monitor = monitorFor(id, assign);
        if (!monitor)
          continue;
        const bool exists = workspaceOf(id) != nullptr;
        if (!target && !exists)
          continue;
        const bool resident = exists || assign.preload;
        const int  priority = (visible(id) ? 0 : resident ? (target ? 100 : 200) : 1000) + (int)std::clamp<WORKSPACEID>(id, 0, 99);

        SKey key{assign.picture, sizeOf(monitor)};
        SNeed need{key, priority, resident, LEVEL_FULL};
        if (!target) {
          // Kept as it is, outside the budget.
          const auto have = g_state->textures.find(key);
          need.level      = have != g_state->textures.end() ? have->second.level : LEVEL_FULL;
          need.priority += 10000;
        }
        auto [it, created] = merged.try_emplace(key, need);
        if (!created) {
          it->second.priority = std::min(it->second.priority, priority);
          it->second.resident |= resident;
        }
      }
    };

    const auto& target = g_state->pending ? *g_state->pending : g_state->shown;
    add(target, true);
    if (g_state->pending)
      add(g_state->shown, false);

    // A workspace whose picture failed keeps showing the one it had, at
    // whatever size its monitor has now: a new mode or scale must not lose it.
    SAssignments fallbacks;
    for (const auto& [id, view] : g_state->views) {
      const auto it = target.find(id);
      if (view.current.empty() || it == target.end() || it->second.picture == view.current)
        continue;
      if (it->second.picture.stamp.empty() || g_state->failed.contains(it->second.picture))
        fallbacks[id] = SAssign{view.current, it->second.monitor, it->second.preload};
    }
    add(fallbacks, true);

    std::vector<SNeed> needs;
    for (auto& [key, need] : merged)
      needs.push_back(need);
    std::ranges::sort(needs, [](const SNeed& a, const SNeed& b) { return std::tie(a.priority, a.key) < std::tie(b.priority, b.key); });

    // The budget: shrink the least urgent textures first, and the ones on
    // screen last, a level at a time, rather than leave any out.
    const auto inBudget = [](const SNeed& n) { return n.resident && n.priority < 10000; };
    uint64_t   total    = 0;
    for (const auto& n : needs)
      total += inBudget(n) ? bytesOf(n.key.size) : 0;
    for (int pass = 0; pass < 4 && total > g_state->config.budget; ++pass) {
      const eLevel to       = pass < 2 ? LEVEL_HALF : LEVEL_QUARTER;
      const bool   onScreen = pass % 2 == 1;
      for (auto it = needs.rbegin(); it != needs.rend() && total > g_state->config.budget; ++it) {
        if (!inBudget(*it) || it->level >= to || (it->priority < 100) != onScreen)
          continue;
        total -= bytesOf(atLevel(it->key.size, it->level));
        it->level = to;
        total += bytesOf(atLevel(it->key.size, it->level));
      }
    }

    return needs;
  }

  // Textures the views draw now: kept until the next commit or prune frees them.
  std::set<SKey> onScreenKeys() {
    std::set<SKey> keys;
    for (const auto& m : monitors()) {
      for (const auto& [id, view] : g_state->views) {
        const auto ws = workspaceOf(id);
        if (ws && ws->m_monitor.lock() != m)
          continue;
        keys.insert(SKey{view.current, sizeOf(m)});
        if (!view.previous.empty())
          keys.insert(SKey{view.previous, sizeOf(m)});
      }
    }
    return keys;
  }

  // Swap the set being prepared in, all at once, once every picture it needs
  // is uploaded or has failed, and never while a workspace is moving, so a
  // theme switch neither shows half its pictures nor changes one mid-slide.
  bool pendingReady() {
    for (const auto& [id, assign] : *g_state->pending) {
      if (assign.picture.stamp.empty() || g_state->failed.contains(assign.picture))
        continue; // keeps what it has
      const auto monitor = monitorFor(id, assign);
      if (!monitor || !(assign.preload || workspaceOf(id)))
        continue;
      if (!g_state->textures.contains(SKey{assign.picture, sizeOf(monitor)}))
        return false;
    }
    return true;
  }

  void commit() {
    auto& next = *g_state->pending;
    for (auto it = g_state->views.begin(); it != g_state->views.end();) {
      if (!next.contains(it->first))
        it = g_state->views.erase(it);
      else
        ++it;
    }

    for (const auto& [id, assign] : next) {
      auto picture = assign.picture;
      // A picture that cannot be shown keeps the one the workspace had.
      if (g_state->failed.contains(picture) || picture.stamp.empty())
        continue;

      auto [it, created] = g_state->views.try_emplace(id);
      auto& view         = it->second;
      if (created) {
        Animation::mgr()->createAnimation(1.F, view.fade, Config::animationTree()->getAnimationPropertyConfig("fadeIn"), AVARDAMAGE_NONE);
        view.fade->setUpdateCallback([](auto) { markChanged(); });
        view.fade->setCallbackOnEnd(
            [id](auto) {
              if (!g_state || !g_state->views.contains(id))
                return;
              auto& v = g_state->views[id];
              if (v.fade->value() >= 1.F)
                v.previous = {};
              schedule();
            },
            false);
      }
      if (view.current == picture)
        continue;

      // Only a workspace on screen fades; the rest change unseen.
      if (visible(id) && !created && !view.current.empty()) {
        view.previous = view.current;
        view.current  = picture;
        view.fade->setValueAndWarp(0.F);
        *view.fade = 1.F;
      } else {
        view.current  = picture;
        view.previous = {};
        if (created && visible(id)) {
          view.fade->setValueAndWarp(0.F);
          *view.fade = 1.F;
        }
      }
    }

    g_state->shown = std::move(next);
    g_state->pending.reset();
    g_state->commits++;
    markChanged();
  }

  void pruneTextures() {
    std::set<SKey> keep = onScreenKeys();
    for (const auto& n : g_state->needs) {
      if (n.resident)
        keep.insert(n.key);
    }
    std::erase_if(g_state->textures, [&](const auto& e) {
      if (!keep.contains(e.first))
        return true;
      // Superseded by the level the budget wants, once that one is in.
      return false;
    });
  }

  // Keeps the disk cache under its cap, oldest first, never touching what is
  // wanted now. Left-over partial files from killed helpers go too.
  void pruneDisk() {
    std::set<std::string> wanted;
    for (const auto& n : g_state->needs)
      wanted.insert(std::filesystem::path{cacheFile(n.key)}.filename().string());

    struct SEntry {
      std::filesystem::path         path;
      uint64_t                      size;
      std::filesystem::file_time_type time;
    };
    std::vector<SEntry> entries;
    uint64_t            total = 0;
    std::error_code     ec;
    const auto          now = std::filesystem::file_time_type::clock::now();
    for (const auto& e : std::filesystem::directory_iterator(g_state->config.cacheDir, ec)) {
      const auto name = e.path().filename().string();
      if (!e.is_regular_file(ec))
        continue;
      const auto time = e.last_write_time(ec);
      if (name.ends_with(".tmp")) {
        if (now - time > std::chrono::minutes(2))
          std::filesystem::remove(e.path(), ec);
        continue;
      }
      if (!name.ends_with(".pix"))
        continue;
      const auto size = e.file_size(ec);
      total += size;
      if (!wanted.contains(name))
        entries.push_back({e.path(), size, time});
    }
    std::ranges::sort(entries, [](const SEntry& a, const SEntry& b) { return a.time < b.time; });
    for (const auto& e : entries) {
      if (total <= g_state->config.cacheCap)
        break;
      std::filesystem::remove(e.path, ec);
      total -= e.size;
    }
  }

  // Works out what is wanted and gets it going: helpers for pictures not on
  // disk yet, reads for textures not on the GPU yet, in order of urgency, and
  // stops whatever a newer plan made obsolete before it is ever uploaded.
  void replan() {
    if (!g_state)
      return;

    g_state->needs = computeNeeds();
    g_state->sizes.clear();
    for (const auto& m : monitors())
      g_state->sizes[m->m_id] = sizeOf(m);

    std::set<SKey> wanted;
    for (const auto& n : g_state->needs)
      wanted.insert(n.key);

    // Helpers working on something no longer wanted are stopped.
    for (auto it = g_state->helpers.begin(); it != g_state->helpers.end();) {
      if (!wanted.contains(it->first)) {
        killHelper(*it->second);
        g_state->helpersStopped++;
        g_state->disk.erase(it->first);
        it = g_state->helpers.erase(it);
      } else
        ++it;
    }

    // Reads still queued for something no longer wanted are dropped; one
    // already running finishes and is thrown away.
    std::vector<SKey> dropped;
    {
      std::scoped_lock lock(g_state->mutex);
      std::erase_if(g_state->queue, [&](SLoad* load) {
        const auto need = needFor(load->key);
        if (need && need->resident && need->level == load->level)
          return false;
        dropped.push_back(load->key);
        return true;
      });
    }
    for (const auto& key : dropped) {
      g_state->loads.erase(key);
      g_state->discarded++;
    }
    std::erase_if(g_state->uploads, [](const SP<SLoad>& load) {
      const auto need = needFor(load->key);
      if (!need || !need->resident || need->level != load->level) {
        g_state->discarded++;
        return true;
      }
      return false;
    });

    for (const auto& n : g_state->needs) {
      auto& disk = g_state->disk[n.key];
      if (disk == DISK_MISSING && fileComplete(cacheFile(n.key), n.key))
        disk = DISK_READY;

      if (disk == DISK_MISSING) {
        if ((int)g_state->helpers.size() < g_state->config.helpers)
          startHelper(n.key);
        continue;
      }
      if (disk != DISK_READY || !n.resident)
        continue;

      const auto tex = g_state->textures.find(n.key);
      if (tex != g_state->textures.end() && tex->second.level == n.level)
        continue;
      const auto loading = g_state->loads.find(n.key);
      if (loading != g_state->loads.end() && loading->second->level == n.level)
        continue;
      if (std::ranges::any_of(g_state->uploads, [&](const SP<SLoad>& l) { return l->key == n.key && l->level == n.level; }))
        continue;
      if (loading != g_state->loads.end())
        continue; // an obsolete read still running; this one starts when it is done
      startLoad(n);
    }

    // Reads go in order of urgency.
    {
      std::scoped_lock lock(g_state->mutex);
      std::ranges::stable_sort(g_state->queue, [](SLoad* a, SLoad* b) {
        const auto na = needFor(a->key), nb = needFor(b->key);
        return (na ? na->priority : INT_MAX) < (nb ? nb->priority : INT_MAX);
      });
    }

    schedule();
  }

  // The quiet-moment work: one upload at a time while nothing this plugin
  // draws is animating, then swapping a prepared set in, then pruning.
  void onTimer() {
    if (!g_state)
      return;

    // A helper that has not finished in a minute is stuck; it is its own
    // process, so it is simply stopped.
    for (auto& [key, helper] : g_state->helpers) {
      if (Clock::now() - helper->started > std::chrono::seconds(60) && helper->pidfd >= 0) {
        pidfd_send_signal(helper->pidfd, SIGKILL, nullptr, 0);
        helper->message = "the image helper took over a minute and was stopped";
      }
    }

    const bool isBusy = busy();
    if (!isBusy && !g_state->uploads.empty()) {
      std::ranges::stable_sort(g_state->uploads, [](const SP<SLoad>& a, const SP<SLoad>& b) {
        const auto na = needFor(a->key), nb = needFor(b->key);
        return (na ? na->priority : INT_MAX) < (nb ? nb->priority : INT_MAX);
      });
      const auto load = g_state->uploads.front();
      g_state->uploads.pop_front();
      upload(load, false);
    }

    if (g_state->pending && !workspacesMoving() && pendingReady()) {
      commit();
      replan();
      return;
    }

    if (!isBusy && !g_state->pending) {
      pruneTextures();
      static auto lastDiskPrune = Clock::time_point{};
      if (Clock::now() - lastDiskPrune > std::chrono::seconds(30)) {
        lastDiskPrune = Clock::now();
        pruneDisk();
      }
    }

    if (!g_state->uploads.empty() || g_state->pending || !g_state->helpers.empty())
      schedule(std::chrono::milliseconds(isBusy ? 16 : 4));
  }

  void schedule(std::chrono::milliseconds in) {
    if (!g_state || !g_state->timer)
      return;
    if (g_state->timer->armed() && g_state->timer->leftUs() <= in.count() * 1000.F)
      return;
    g_state->timer->updateTimeout(in);
  }

  // -------------------------------------------------------------------- draw

  // Drawn as an opaque quad, with blending off: the picture covers its
  // workspace edge to edge, so nothing under it needs to be read back.
  class CBackgroundElement : public IPassElement {
    public:
    CBackgroundElement(SP<Render::ITexture> tex, const CBox& box, float alpha, bool plain) : m_tex(tex), m_box(box), m_alpha(alpha), m_plain(plain) {}

    std::vector<UP<IPassElement>> draw() override {
      const bool opaque = m_alpha >= 1.F;
      if (opaque)
        g_pHyprRenderer->blend(false);
      Render::GL::g_pHyprOpenGL->renderTexture(m_tex, m_box, {.a = m_alpha});
      if (opaque)
        g_pHyprRenderer->blend(true);
      return {};
    }
    bool needsLiveBlur() override {
      return false;
    }
    bool needsPrecomputeBlur() override {
      return false;
    }
    const char* passName() override {
      return "CWorkspaceBackgroundElement";
    }
    ePassElementType type() override {
      return EK_CUSTOM;
    }
    std::optional<CBox> boundingBox() override {
      const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
      return m_box.copy().scale(1.F / (monitor ? monitor->m_scale : 1.F)).round();
    }
    // What is under an opaque picture is skipped, the background layer
    // included, unless the frame is a scaled or moved copy (an overview).
    CRegion opaqueRegion() override {
      if (m_alpha < 1.F || !m_plain)
        return {};
      const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
      return CRegion{m_box.copy().scale(1.F / (monitor ? monitor->m_scale : 1.F)).expand(-1).round()};
    }

    private:
    SP<Render::ITexture> m_tex;
    CBox                 m_box;
    float                m_alpha = 1.F;
    bool                 m_plain = true;
  };

  // The texture of a picture at this monitor's size, at whatever level it is.
  Render::ITexture* textureOf(const SPicture& picture, SSize size, SP<Render::ITexture>* out = nullptr) {
    const auto it = g_state->textures.find(SKey{picture, size});
    if (it == g_state->textures.end())
      return nullptr;
    if (out)
      *out = it->second.tex;
    return it->second.tex.get();
  }

  void drawView(PHLMONITOR monitor, const SView& view, const Vector2D& logicalOffset, float alpha, bool plain, std::vector<SDrawn>* drawn) {
    const auto size   = sizeOf(monitor);
    const auto offset = logicalOffset * monitor->m_scale;
    const CBox box    = CBox{std::round(offset.x), std::round(offset.y), (double)size.w, (double)size.h};

    const auto draw = [&](const SPicture& picture, float a) {
      SP<Render::ITexture> tex;
      if (picture.empty() || a <= 0.F || !textureOf(picture, size, &tex))
        return false;
      g_pHyprRenderer->m_renderPass.add(makeUnique<CBackgroundElement>(tex, box, a, plain));
      if (drawn)
        drawn->push_back({tex.get(), box, a});
      return true;
    };

    const float fade = view.fade ? view.fade->value() : 1.F;
    if (fade < 1.F || !textureOf(view.current, size)) {
      // Under the fade, or instead of a picture that is not on the GPU yet.
      draw(view.previous, alpha);
    }
    // Fades in over the old picture, or over the background layer when there
    // was none; with the old one missing at this size, it simply shows.
    draw(view.current, alpha * (view.previous.empty() || textureOf(view.previous, size) ? fade : 1.F));
  }

  const SView* viewFor(WORKSPACEID id) {
    const auto it = g_state->views.find(id);
    return it == g_state->views.end() ? nullptr : &it->second;
  }

  Vector2D sideOf(const Vector2D& offset) {
    if (offset == Vector2D{})
      return {};
    return std::abs(offset.x) >= std::abs(offset.y) ? Vector2D{offset.x > 0 ? 1.0 : -1.0, 0.0} : Vector2D{0.0, offset.y > 0 ? 1.0 : -1.0};
  }

  // How far apart a swipe keeps adjacent workspaces: one monitor plus
  // `gaps_workspaces`, whatever the switch style.
  Vector2D swipeDistance(PHLMONITOR monitor) {
    static auto PWORKSPACEGAP = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
    return monitor->m_size + Vector2D{(double)*PWORKSPACEGAP, (double)*PWORKSPACEGAP};
  }

  struct SPlanned {
    WORKSPACEID id = WORKSPACE_INVALID;
    Vector2D    offset;
    float       alpha = 1.F;
  };

  // Hyprland destroys an empty workspace the moment it is left, so there is
  // nothing left to slide out, but its background still has to. A keyboard
  // switch is followed from its own animation (SLeaving); a swipe is released
  // without one, and both workspaces continue from where the fingers left
  // them, one swipe distance apart.
  void planSwipeLeaving(PHLMONITOR monitor, const PHLWORKSPACE& active, STrack& track, std::vector<SPlanned>& out) {
    if (track.swipeLeaving == WORKSPACE_INVALID)
      return;

    if (!moving(active) || !track.leavingFromSwipe) {
      track.swipeLeaving = WORKSPACE_INVALID;
      return;
    }

    // Still alive, so it draws itself where Hyprland puts it.
    if (workspaceOf(track.swipeLeaving))
      return;

    // Fixed on the first frame that has one, so a spring curve overshooting
    // zero cannot flip it.
    const auto offset = active->m_renderOffset->value();
    if (track.side == Vector2D{})
      track.side = sideOf(offset);
    out.push_back({track.swipeLeaving, offset - track.side * swipeDistance(monitor), 1.F});
  }

  void planKeyboardLeaving(PHLMONITOR monitor, STrack& track, std::vector<SPlanned>& out) {
    std::erase_if(track.leaving, [](const SLeaving& l) { return !l.offset->isBeingAnimated() && !l.alpha->isBeingAnimated(); });
    for (const auto& l : track.leaving) {
      if (!workspaceOf(l.id))
        out.push_back({l.id, l.offset->value(), l.alpha->value()});
    }
  }

  // A swipe toward a workspace that does not exist yet moves the current one
  // over empty space, and Hyprland creates the new workspace only when the
  // fingers lift. Its background is drawn where that workspace will be,
  // found the way Hyprland will find it, so it follows the fingers too, and
  // slides back out with the current workspace when the swipe is let go short.
  WORKSPACEID swipeTargetOf(const PHLWORKSPACE& active, const Vector2D& side) {
    static auto PSWIPENEW  = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_create_new");
    static auto PSWIPEUSER = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_use_r");
    const bool  toRight    = side.x + side.y < 0;
    WORKSPACEID target     = getWorkspaceIDNameFromString(toRight ? (*PSWIPEUSER ? "r+1" : "m+1") : (*PSWIPEUSER ? "r-1" : "m-1")).id;
    if (toRight && *PSWIPENEW && target <= active->m_id)
      target = getWorkspaceIDNameFromString("r+1").id;
    return target;
  }

  void planSwipeTarget(PHLMONITOR monitor, const PHLWORKSPACE& active, STrack& track, bool swiping, std::vector<SPlanned>& out) {
    const auto offset = active->m_renderOffset->value();

    if (swiping) {
      track.swipeTarget = WORKSPACE_INVALID;
      const auto side   = sideOf(offset);
      if (side == Vector2D{})
        return;

      const auto target = swipeTargetOf(active, side);
      if (target == WORKSPACE_INVALID || target == active->m_id || workspaceOf(target))
        return;

      track.swipeTarget = target;
      track.swipeBegin  = active->m_id;
      track.swipeSide   = side;
    } else if (track.swipeTarget == WORKSPACE_INVALID || track.swipeBegin != active->m_id || !active->m_renderOffset->isBeingAnimated() || workspaceOf(track.swipeTarget)) {
      track.swipeTarget = WORKSPACE_INVALID;
      return;
    }

    out.push_back({track.swipeTarget, offset - track.swipeSide * swipeDistance(monitor), 1.F});
  }

  // A swipe let go past halfway toward a workspace that did not exist yet
  // creates it already in place, while the one left slides out over it. The
  // new background rides in beside the one leaving instead of appearing at once.
  Vector2D arrivingOffset(PHLMONITOR monitor, const PHLWORKSPACE& active, const STrack& track) {
    if (!track.leavingFromSwipe || moving(active))
      return active->m_renderOffset->value();

    for (const auto& workspace : State::workspaceState()->workspaces()) {
      const auto ws = workspace.lock();
      if (!ws || ws == active || ws->m_isSpecialWorkspace || ws->m_monitor != monitor || !ws->m_renderOffset->isBeingAnimated())
        continue;

      const auto side = sideOf(ws->m_renderOffset->goal());
      if (side != Vector2D{})
        return ws->m_renderOffset->value() - side * swipeDistance(monitor);
    }

    return active->m_renderOffset->value();
  }

  // What one monitor's frame shows, back to front. Advances the per-monitor
  // tracking, so it runs once per frame.
  std::vector<SPlanned> planFrame(PHLMONITOR monitor) {
    std::vector<SPlanned> out;
    const auto            active = monitor->m_activeWorkspace;
    if (!active)
      return out;

    const bool swiping = active->m_forceRendering && g_pUnifiedWorkspaceSwipe->isGestureInProgress() && Desktop::focusState()->monitor() == monitor;

    auto&      track = g_state->tracks[monitor->m_id];
    if (track.active != active->m_id) {
      track.swipeLeaving     = track.active;
      track.active           = active->m_id;
      track.side             = {};
      track.leavingFromSwipe = track.swiping;
    }
    track.swiping = swiping;
    track.current = active;

    planKeyboardLeaving(monitor, track, out);
    planSwipeLeaving(monitor, active, track, out);
    planSwipeTarget(monitor, active, track, swiping, out);

    // Workspaces on their way in or out, under the active one, so it is drawn
    // over them when the switch style overlaps them.
    for (const auto& workspace : State::workspaceState()->workspaces()) {
      const auto ws = workspace.lock();
      if (!ws || ws == active || ws->m_isSpecialWorkspace || ws->m_monitor != monitor || !moving(ws))
        continue;
      out.push_back({ws->m_id, ws->m_renderOffset->value(), ws->m_alpha->value()});
    }

    out.push_back({active->m_id, arrivingOffset(monitor, active, track), active->m_alpha->value()});
    return out;
  }

  // Before a frame: whatever it is about to show gets its texture now if it
  // has none, and Hyprland's cached blur of the background is marked stale
  // whenever the background will differ, so see-through windows never show
  // the last workspace's picture, blurred.
  void onRenderPreChecks(PHLMONITOR monitor) {
    if (!g_state || !monitor || !monitor->m_activeWorkspace)
      return;

    // A new mode, scale or rotation does not always come with a monitor
    // event; every frame knows the size it is drawn at.
    const auto size = sizeOf(monitor);
    if (const auto it = g_state->sizes.find(monitor->m_id); it == g_state->sizes.end() || it->second != size)
      replan();
    if (g_state->views.empty())
      return;

    const auto active = monitor->m_activeWorkspace;
    auto       need   = [&](WORKSPACEID id) {
      if (const auto view = viewFor(id); view && !view->current.empty())
        loadNow(SKey{view->current, size});
    };
    need(active->m_id);
    for (const auto& workspace : State::workspaceState()->workspaces()) {
      const auto ws = workspace.lock();
      if (ws && !ws->m_isSpecialWorkspace && ws->m_monitor == monitor && moving(ws))
        need(ws->m_id);
    }
    if (g_pUnifiedWorkspaceSwipe->isGestureInProgress() && active->m_forceRendering) {
      for (const auto side : {Vector2D{-1, 0}, Vector2D{1, 0}})
        need(swipeTargetOf(active, side));
    }

    bool animating = g_pUnifiedWorkspaceSwipe->isGestureInProgress();
    for (const auto& workspace : State::workspaceState()->workspaces()) {
      const auto ws = workspace.lock();
      animating = animating || (ws && ws->m_monitor == monitor && !ws->m_isSpecialWorkspace && moving(ws));
    }
    for (const auto& [id, view] : g_state->views)
      animating = animating || (view.fade && view.fade->isBeingAnimated());
    const auto& track = g_state->tracks[monitor->m_id];
    if (animating || g_state->changed.contains(monitor->m_id) || track.swipeLeaving != WORKSPACE_INVALID || !track.leaving.empty())
      monitor->m_blurFBDirty = true;
    g_state->changed.erase(monitor->m_id);
  }

  void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WALLPAPER || !g_state || g_state->views.empty())
      return;

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor || !monitor->m_activeWorkspace)
      return;

    // Something other than the monitor's own frame, an overview drawing each
    // workspace into a tile say: just that workspace's picture, where it is.
    const auto& r = g_state->rendering;
    if (r.active && r.workspace && (r.workspace != monitor->m_activeWorkspace || !r.plain)) {
      if (const auto view = viewFor(r.workspace->m_id))
        drawView(monitor, *view, {}, 1.F, false, nullptr);
      return;
    }

    std::vector<SDrawn> drawn;
    for (const auto& p : planFrame(monitor)) {
      if (const auto view = viewFor(p.id))
        drawView(monitor, *view, p.offset, p.alpha, true, &drawn);
    }

    // Changed in a way nothing predicted: this frame's blur may lag, so the
    // next one is drawn and refreshes it.
    auto& last = g_state->drawn[monitor->m_id];
    if (drawn != last) {
      monitor->m_blurFBDirty = true;
      if (!g_state->changed.contains(monitor->m_id)) {
        g_state->changed.insert(monitor->m_id);
        g_pHyprRenderer->damageMonitor(monitor);
      }
      last = std::move(drawn);
    }
  }

  // ------------------------------------------------------------------ inputs

  typedef void (*renderAllClientsForWorkspaceFn)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const Vector2D&, const float&);

  void hkRenderAllClientsForWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, const Vector2D& translate, const float& scale) {
    auto&      r     = g_state->rendering;
    const auto saved = r;
    r.active         = true;
    r.workspace      = workspace;
    r.plain          = translate == Vector2D{} && scale == 1.F;
    ((renderAllClientsForWorkspaceFn)g_state->renderHook->m_original)(thisptr, monitor, workspace, now, translate, scale);
    g_state->rendering = saved;
  }

  // A keyboard switch has just started both workspaces' animations. If the one
  // left is about to be destroyed, its picture takes over its animation.
  void onWorkspaceActive(PHLWORKSPACE arriving) {
    if (!arriving || arriving->m_isSpecialWorkspace)
      return;
    const auto monitor = arriving->m_monitor.lock();
    if (!monitor)
      return;
    auto&      track = g_state->tracks[monitor->m_id];
    const auto old   = track.current.lock();
    track.current    = arriving;
    if (!old || old == arriving || old->m_isSpecialWorkspace || !viewFor(old->m_id))
      return;
    if (!old->m_renderOffset->isBeingAnimated() && !old->m_alpha->isBeingAnimated())
      return;

    std::erase_if(track.leaving, [&](const SLeaving& l) { return l.id == old->m_id; });
    SLeaving l;
    l.id = old->m_id;
    Animation::mgr()->createAnimation(old->m_renderOffset->begun(), l.offset, old->m_renderOffset->getConfig().lock(), AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(old->m_alpha->begun(), l.alpha, old->m_alpha->getConfig().lock(), AVARDAMAGE_NONE);
    l.offset->setUpdateCallback([](auto) { markChanged(); });
    *l.offset = old->m_renderOffset->goal();
    *l.alpha  = old->m_alpha->goal();
    track.leaving.push_back(std::move(l));
    replan();
  }

  void assign(SAssignments&& wanted) {
    // The same as what is shown or already being prepared: nothing to do,
    // which is every config reload.
    if (g_state->pending ? *g_state->pending == wanted : g_state->shown == wanted)
      return;

    // A newer set replaces one still being prepared; its work is dropped.
    g_state->pending = std::move(wanted);
    replan();
  }

  // hl.plugin.workspace_backgrounds.set({
  //   { workspace = 1, path = "/path/a.webp", monitor = "DP-1", preload = true },
  //   ...
  // })
  //
  // Replaces every assignment at once. The new set is prepared out of sight
  // and swapped in whole once its pictures are ready. A workspace left out
  // shows the ordinary background layer; an empty table hands every workspace
  // back. `monitor` places a workspace that does not exist yet; `preload` keeps
  // its picture on the GPU even while the workspace does not exist, so
  // switching there is instant.
  int luaSet(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    // Checked before any C++ object exists: a Lua error unwinds past them.
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        if (!lua_isinteger(L, -2))
          return luaL_error(L, "workspace_backgrounds.set: { [workspace id] = path } or a list of { workspace = id, path = ... }");
      } else if (lua_type(L, -1) == LUA_TTABLE) {
        lua_getfield(L, -1, "workspace");
        lua_getfield(L, -2, "path");
        lua_getfield(L, -3, "monitor");
        lua_getfield(L, -4, "preload");
        if (!lua_isinteger(L, -4) || lua_type(L, -3) != LUA_TSTRING || !(lua_isnil(L, -2) || lua_type(L, -2) == LUA_TSTRING) || !(lua_isnil(L, -1) || lua_isboolean(L, -1)))
          return luaL_error(L, "workspace_backgrounds.set: each entry needs workspace (integer) and path (string), and optionally monitor (string) and preload (boolean)");
        lua_pop(L, 4);
      } else
        return luaL_error(L, "workspace_backgrounds.set: values must be paths or entries");
      lua_pop(L, 1);
    }

    SAssignments wanted;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
      if (lua_type(L, -1) == LUA_TSTRING) {
        const std::string path = lua_tostring(L, -1);
        if (!path.empty())
          wanted[lua_tointeger(L, -2)] = SAssign{pictureAt(path), "", false};
      } else {
        lua_getfield(L, -1, "workspace");
        lua_getfield(L, -2, "path");
        lua_getfield(L, -3, "monitor");
        lua_getfield(L, -4, "preload");
        const WORKSPACEID id      = lua_tointeger(L, -4);
        const std::string path    = lua_tostring(L, -3);
        const std::string monitor = lua_isnil(L, -2) ? "" : lua_tostring(L, -2);
        const bool        preload = lua_toboolean(L, -1);
        lua_pop(L, 4);
        if (!path.empty())
          wanted[id] = SAssign{pictureAt(path), monitor, preload};
      }
      lua_pop(L, 1);
    }

    assign(std::move(wanted));
    return 0;
  }

  // hl.plugin.workspace_backgrounds.configure({ budget_mb = 384, cache_mb = 1024, helpers = 2, cache_dir = "...", helper = "..." })
  int luaConfigure(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "budget_mb");
    lua_getfield(L, 1, "cache_mb");
    lua_getfield(L, 1, "helpers");
    lua_getfield(L, 1, "cache_dir");
    lua_getfield(L, 1, "helper");
    for (int i = -5; i <= -3; ++i) {
      if (!lua_isnil(L, i) && (!lua_isinteger(L, i) || lua_tointeger(L, i) <= 0))
        return luaL_error(L, "workspace_backgrounds.configure: budget_mb, cache_mb and helpers are positive integers");
    }
    for (int i = -2; i <= -1; ++i) {
      if (!lua_isnil(L, i) && lua_type(L, i) != LUA_TSTRING)
        return luaL_error(L, "workspace_backgrounds.configure: cache_dir and helper are paths");
    }

    auto& c = g_state->config;
    if (!lua_isnil(L, -5))
      c.budget = (uint64_t)lua_tointeger(L, -5) << 20;
    if (!lua_isnil(L, -4))
      c.cacheCap = (uint64_t)lua_tointeger(L, -4) << 20;
    if (!lua_isnil(L, -3))
      c.helpers = std::clamp<int>(lua_tointeger(L, -3), 1, 8);
    if (!lua_isnil(L, -2) && std::string{lua_tostring(L, -2)} != c.cacheDir) {
      c.cacheDir = lua_tostring(L, -2);
      g_state->disk.clear();
    }
    if (!lua_isnil(L, -1))
      c.helper = lua_tostring(L, -1);
    lua_pop(L, 5);

    replan();
    return 0;
  }

  std::string jsonString(const std::string& in) {
    std::string out = "\"";
    for (const unsigned char c : in) {
      if (c == '"' || c == '\\')
        out += std::string{'\\', (char)c};
      else if (c < 0x20)
        out += std::format("\\u{:04x}", c);
      else
        out += (char)c;
    }
    return out + "\"";
  }

  // hyprctl workspacebackgrounds [-j]: each workspace's picture, what it
  // shows now, and the texture memory in use, so scripts and tests read what
  // is drawn rather than work it out again.
  std::string hyprctlWorkspaceBackgrounds(eHyprCtlOutputFormat format, std::string) {
    const auto& latest = g_state->pending ? *g_state->pending : g_state->shown;

    uint64_t bytes = 0;
    for (const auto& [key, tex] : g_state->textures)
      bytes += tex.bytes;

    if (format != eHyprCtlOutputFormat::FORMAT_JSON) {
      std::string out;
      for (const auto& [id, assign] : latest) {
        const auto failed = g_state->failed.find(assign.picture);
        const auto view   = viewFor(id);
        const bool ready  = view && view->current == assign.picture;
        out += std::format("{}\t{}{}\n", id, assign.picture.path, failed != g_state->failed.end() ? "\t(unreadable: " + failed->second + ")" : ready ? "" : "\t(loading)");
      }
      out += std::format("textures: {} ({:.1f} MB of a {:.0f} MB budget){}\n", g_state->textures.size(), bytes / 1048576.0, g_state->config.budget / 1048576.0,
                         g_state->pending ? ", preparing a new set" : "");
      return out;
    }

    std::string workspaces;
    for (const auto& [id, assign] : latest) {
      const auto  failed = g_state->failed.find(assign.picture);
      const auto  view   = viewFor(id);
      const auto  ws     = workspaceOf(id);
      const auto  mon    = monitorFor(id, assign);
      std::string level  = "none";
      if (view && mon) {
        if (const auto it = g_state->textures.find(SKey{view->current, sizeOf(mon)}); it != g_state->textures.end())
          level = levelName(it->second.level);
      }
      workspaces += std::format("{}{{\"workspace\": {}, \"path\": {}, \"drawn\": {}, \"ready\": {}, \"failed\": {}, \"error\": {}, \"preload\": {}, \"exists\": {}, \"monitor\": {}, \"level\": \"{}\"}}",
                                workspaces.empty() ? "" : ",\n    ", id, jsonString(assign.picture.path), jsonString(view ? view->current.path : ""),
                                view && view->current == assign.picture, failed != g_state->failed.end(),
                                jsonString(failed != g_state->failed.end() ? failed->second : ""), assign.preload, ws != nullptr, jsonString(mon ? mon->m_name : ""), level);
    }

    std::string textures;
    for (const auto& [key, tex] : g_state->textures)
      textures += std::format("{}{{\"path\": {}, \"width\": {}, \"height\": {}, \"level\": \"{}\", \"bytes\": {}}}", textures.empty() ? "" : ",\n    ", jsonString(key.picture.path),
                              key.size.w, key.size.h, levelName(tex.level), tex.bytes);

    return std::format("{{\n  \"workspaces\": [\n    {}\n  ],\n  \"textures\": [\n    {}\n  ],\n  \"bytes\": {},\n  \"budget\": {},\n  \"pending\": {},\n  \"helpers\": {},\n"
                       "  \"loading\": {},\n  \"uploads_waiting\": {},\n  \"uploads\": {},\n  \"urgent_uploads\": {},\n  \"last_upload_ms\": {:.2f},\n  \"max_upload_ms\": {:.2f},\n"
                       "  \"uploads_in_motion\": {},\n  \"commits\": {},\n  \"discarded\": {},\n  \"helpers_started\": {},\n  \"helpers_stopped\": {},\n  \"helper\": {},\n  \"helper_ok\": {},\n  \"cache_dir\": {},\n  \"overview_hook\": {}\n}}\n",
                       workspaces, textures, bytes, g_state->config.budget, g_state->pending.has_value(), g_state->helpers.size(), g_state->loads.size(), g_state->uploads.size(),
                       g_state->uploadsDone, g_state->urgentUploads, g_state->lastUploadMs, g_state->maxUploadMs, g_state->uploadsInMotion, g_state->commits, g_state->discarded,
                       g_state->helpersStarted, g_state->helpersStopped,
                       jsonString(g_state->config.helper), access(g_state->config.helper.c_str(), X_OK) == 0, jsonString(g_state->config.cacheDir),
                       g_state->renderHook != nullptr);
  }

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
  return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;

  if (std::string{__hyprland_api_get_hash()} != std::string{__hyprland_api_get_client_hash()}) {
    HyprlandAPI::addNotification(PHANDLE, "[workspace-backgrounds] Built for a different Hyprland; not loaded", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error("[workspace-backgrounds] version mismatch");
  }

  g_state                   = makeUnique<SState>();
  g_state->config.helper    = defaultHelper();
  g_state->config.cacheDir  = defaultCacheDir();
  g_state->worker           = std::thread(workerLoop);
  g_state->timer            = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { onTimer(); }, nullptr);
  g_pEventLoopManager->addTimer(g_state->timer);

  auto& events = Event::bus()->m_events;
  g_state->listeners.emplace_back(events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); }));
  g_state->listeners.emplace_back(events.render.preChecks.listen([](PHLMONITOR m) { onRenderPreChecks(m); }));
  for (auto* signal : {&events.monitor.added, &events.monitor.removed})
    g_state->listeners.emplace_back(signal->listen([](PHLMONITOR) { replan(); }));
  g_state->listeners.emplace_back(events.monitor.layoutChanged.listen([] { replan(); }));
  g_state->listeners.emplace_back(events.workspace.created.listen([](PHLWORKSPACEREF) { replan(); }));
  g_state->listeners.emplace_back(events.workspace.removed.listen([](PHLWORKSPACEREF) { replan(); }));
  g_state->listeners.emplace_back(events.workspace.active.listen([](PHLWORKSPACE ws) { onWorkspaceActive(ws); replan(); }));
  g_state->listeners.emplace_back(events.workspace.moveToMonitor.listen([](PHLWORKSPACE, PHLMONITOR) { replan(); }));

  // Which workspace a frame is drawing, for overview plugins that draw
  // several. Without it, every frame is taken to be the monitor's own.
  const auto found = HyprlandAPI::findFunctionsByName(PHANDLE, "renderAllClientsForWorkspace");
  for (const auto& f : found) {
    if (f.demangled.contains("IHyprRenderer::renderAllClientsForWorkspace")) {
      g_state->renderHook = HyprlandAPI::createFunctionHook(PHANDLE, f.address, (void*)&hkRenderAllClientsForWorkspace);
      if (g_state->renderHook && !g_state->renderHook->hook()) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_state->renderHook);
        g_state->renderHook = nullptr;
      }
      break;
    }
  }

  HyprlandAPI::addLuaFunction(PHANDLE, "workspace_backgrounds", "set", luaSet);
  HyprlandAPI::addLuaFunction(PHANDLE, "workspace_backgrounds", "configure", luaConfigure);
  g_state->hyprctl = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "workspacebackgrounds", .exact = true, .fn = hyprctlWorkspaceBackgrounds});

  return {"workspace_backgrounds", "A background for every workspace, moving with it", "Omarchy", "0.2.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  if (!g_state)
    return;

  {
    std::scoped_lock lock(g_state->mutex);
    g_state->exiting = true;
    g_state->wake.notify_all();
  }
  g_state->worker.join();

  if (g_state->renderHook)
    HyprlandAPI::removeFunctionHook(PHANDLE, g_state->renderHook);
  g_state->renderHook = nullptr;

  for (auto& [key, helper] : g_state->helpers)
    killHelper(*helper);
  g_state->helpers.clear();
  if (g_state->timer) {
    g_state->timer->cancel();
    g_pEventLoopManager->removeTimer(g_state->timer);
  }
  HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_state->hyprctl);

  g_state.reset();
  // The last frame's pass still holds our elements until the next frame
  // clears it; they must go while their code is still loaded.
  g_pHyprRenderer->m_renderPass.removeAllOfType("CWorkspaceBackgroundElement");
  damageAll();
}
