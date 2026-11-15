// Gives every workspace its own background, drawn by the compositor as part of
// the workspace, so it moves with the workspace's own render offset: the slide
// of a workspace switch, a finger-following swipe, and the fade style alike.
//
// A layer-shell wallpaper sits under every workspace at once and cannot know
// where a workspace is mid-switch, so it can only swap after the fact. Drawing
// here, right after the background layer and before the bottom layer, puts the
// picture exactly where Hyprland puts that workspace's windows, every frame.

#define WLR_USE_UNSTABLE

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/MainLoopExecutor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>

#include <hyprgraphics/image/Image.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

  HANDLE PHANDLE = nullptr;

  struct SSize {
    int  w = 0, h = 0;
    auto operator<=>(const SSize&) const = default;
  };

  // A picture is its file as it is now: a theme switch can put a different
  // picture at the same path, which must not be mistaken for the one decoded.
  struct SPicture {
    std::string path;
    std::string stamp;
    auto        operator<=>(const SPicture&) const = default;
    bool        empty() const {
      return path.empty();
    }
  };

  SPicture pictureAt(const std::string& path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0)
      return {path, ""};
    return {path, std::format("{}:{}:{}.{}:{}", st.st_dev, st.st_ino, st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_size)};
  }

  struct SKey {
    SPicture    picture;
    SSize       size;
    auto        operator<=>(const SKey&) const = default;
  };

  // A decode, done off the main thread: Hyprland would otherwise stop drawing
  // for the ~100 ms a large WebP takes. The picture is scaled to cover the
  // monitor and cropped to it there, so the GPU holds exactly one screen of
  // pixels per picture and never draws past its workspace's edge mid-slide.
  struct SJob {
    SKey                  key;
    cairo_surface_t*      result = nullptr;
    std::string           error;
    SP<CMainLoopExecutor> done;
  };

  // One per workspace that was given a background.
  struct SSlot {
    SPicture          picture;
    SPicture          previous; // still drawn under `picture` while it fades in
    bool              pending = true;
    PHLANIMVAR<float> fade;
  };

  struct SState {
    std::map<WORKSPACEID, SSlot>     slots;
    std::map<SKey, SP<Render::ITexture>> textures;
    std::set<SKey>                   failed;
    // Jobs are owned here, on the main thread only: Hyprland's shared
    // pointers count references without atomics, so the worker is only ever
    // handed a plain pointer, and inFlight keeps the job alive until the main
    // thread has finished it. PLUGIN_EXIT joins the worker before freeing any.
    std::map<SKey, SP<SJob>>         inFlight;
    // A finished job's executor is still running the callback that finishes
    // it, so the job is let go of on the next one rather than inside it.
    std::vector<SP<SJob>>            retired;

    std::thread                      worker;
    std::mutex                       mutex;
    std::condition_variable          wake;
    std::deque<SJob*>                queue;
    bool                             exiting = false;

    // What each monitor showed last frame, to tell which workspace is being
    // left and whether a swipe was under way when it was.
    struct STrack {
      WORKSPACEID active           = WORKSPACE_INVALID;
      WORKSPACEID leaving          = WORKSPACE_INVALID;
      Vector2D    side;
      bool        swiping          = false;
      bool        leavingFromSwipe = false;
      WORKSPACEID swipeTarget      = WORKSPACE_INVALID;
      WORKSPACEID swipeBegin       = WORKSPACE_INVALID;
      Vector2D    swipeSide;
    };
    std::map<MONITORID, STrack>      tracks;

    std::vector<CHyprSignalListener> listeners;
    SP<SHyprCtlCommand>              hyprctl;
  };

  UP<SState> g_state;

  SSize sizeOf(PHLMONITOR monitor) {
    return {(int)std::round(monitor->m_transformedSize.x), (int)std::round(monitor->m_transformedSize.y)};
  }

  cairo_surface_t* decode(const SKey& key, std::string& error) {
    Hyprgraphics::CImage image(key.picture.path);
    if (!image.success()) {
      error = image.getError();
      return nullptr;
    }

    cairo_surface_t* source = image.cairoSurface()->cairo();
    const double     sw     = cairo_image_surface_get_width(source);
    const double     sh     = cairo_image_surface_get_height(source);
    if (sw <= 0 || sh <= 0) {
      error = "empty image";
      return nullptr;
    }

    cairo_surface_t* target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, key.size.w, key.size.h);
    cairo_t*         cr     = cairo_create(target);
    const double     scale  = std::max(key.size.w / sw, key.size.h / sh);
    cairo_translate(cr, (key.size.w - sw * scale) / 2.0, (key.size.h - sh * scale) / 2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, source, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(target);
    return target;
  }

  void workerLoop() {
    while (true) {
      SJob* job = nullptr;
      {
        std::unique_lock lock(g_state->mutex);
        g_state->wake.wait(lock, [] { return g_state->exiting || !g_state->queue.empty(); });
        if (g_state->exiting)
          return;
        job = g_state->queue.front();
        g_state->queue.pop_front();
      }

      job->result = decode(job->key, job->error);
      // The last touch: once signalled, the main thread may finish and free it.
      job->done->signal();
    }
  }

  void damageAll() {
    for (const auto& m : State::monitorState()->monitors())
      g_pHyprRenderer->damageMonitor(m);
  }

  std::set<SSize> monitorSizes() {
    std::set<SSize> sizes;
    for (const auto& m : State::monitorState()->monitors()) {
      const auto size = sizeOf(m);
      if (size.w > 0 && size.h > 0)
        sizes.insert(size);
    }
    return sizes;
  }

  // A picture that cannot be read never becomes ready, so the workspace keeps
  // what it shows now rather than dropping to the background layer.
  bool readyEverywhere(const SPicture& picture) {
    for (const auto& size : monitorSizes()) {
      if (!g_state->textures.contains(SKey{picture, size}))
        return false;
    }
    return true;
  }

  // Drop decoded pictures no workspace shows or fades from any more, and
  // pictures for monitor sizes that are gone.
  void prune() {
    std::set<SPicture> pictures;
    for (const auto& [id, slot] : g_state->slots) {
      pictures.insert(slot.picture);
      if (!slot.previous.empty())
        pictures.insert(slot.previous);
    }

    const auto sizes = monitorSizes();
    std::erase_if(g_state->textures, [&](const auto& e) { return !pictures.contains(e.first.picture) || !sizes.contains(e.first.size); });
    std::erase_if(g_state->failed, [&](const auto& key) { return !pictures.contains(key.picture); });
  }

  // A workspace whose new picture is decoded for every monitor fades it in over
  // the one it had, with Hyprland's own fade animation, so `animations off`
  // makes it instant like everything else.
  void startReadyFades() {
    for (auto& [id, slot] : g_state->slots) {
      if (!slot.pending || !readyEverywhere(slot.picture))
        continue;

      slot.pending = false;
      slot.fade->setValueAndWarp(0.F);
      *slot.fade = 1.F;
    }
    damageAll();
  }

  void finishJob(const SKey& key) {
    if (!g_state)
      return;

    const auto it = g_state->inFlight.find(key);
    if (it == g_state->inFlight.end())
      return;

    const auto job = it->second;
    g_state->inFlight.erase(it);
    g_state->retired.clear();
    g_state->retired.push_back(job);

    if (job->result) {
      g_state->textures[key] = g_pHyprRenderer->createTexture(job->result);
      cairo_surface_destroy(job->result);
      job->result = nullptr;
    } else {
      Log::logger->log(Log::ERR, "[workspace-backgrounds] cannot load {}: {}", key.picture.path, job->error);
      g_state->failed.insert(key);
    }

    prune();
    startReadyFades();
  }

  void requestDecodes() {
    const auto sizes = monitorSizes();
    for (const auto& [id, slot] : g_state->slots) {
      for (const auto& size : sizes) {
        SKey key{slot.picture, size};
        if (g_state->textures.contains(key) || g_state->failed.contains(key) || g_state->inFlight.contains(key))
          continue;

        auto job  = makeShared<SJob>();
        job->key  = key;
        job->done = makeShared<CMainLoopExecutor>([key] { finishJob(key); });
        g_state->inFlight[key] = job;

        std::scoped_lock lock(g_state->mutex);
        g_state->queue.push_back(job.get());
        g_state->wake.notify_one();
      }
    }
  }

  void assign(std::map<WORKSPACEID, std::string>&& wanted) {
    // Workspaces no longer named keep nothing, and show the background layer.
    std::erase_if(g_state->slots, [&](const auto& e) { return !wanted.contains(e.first); });

    for (auto& [id, path] : wanted) {
      const auto picture = pictureAt(path);
      auto [it, created] = g_state->slots.try_emplace(id);
      auto& slot         = it->second;

      if (created) {
        Animation::mgr()->createAnimation(1.F, slot.fade, Config::animationTree()->getAnimationPropertyConfig("fadeIn"), AVARDAMAGE_NONE);
        slot.fade->setUpdateCallback([](auto) { damageAll(); });
        slot.fade->setCallbackOnEnd(
            [id](auto) {
              if (!g_state || !g_state->slots.contains(id))
                return;
              auto& s = g_state->slots[id];
              // Warping the fade back to its start ends it too.
              if (!s.pending && s.fade->value() >= 1.F)
                s.previous = {};
              prune();
            },
            false);
      } else if (slot.picture == picture)
        continue;
      else if (!slot.pending)
        slot.previous = slot.picture; // what is on screen now stays under the fade

      slot.picture = picture;
      slot.pending = true;
    }

    requestDecodes();
    prune();
    startReadyFades();
  }

  // hl.plugin.workspace_backgrounds.set({ [1] = "/path/a.webp", [2] = "/path/b.jpg", ... })
  //
  // Replaces every assignment at once. A workspace left out shows the
  // ordinary background layer; an empty table hands every workspace back.
  int luaSet(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    // Checked before any C++ object exists: a Lua error unwinds past them.
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
      if (!lua_isinteger(L, -2))
        return luaL_error(L, "workspace_backgrounds.set: keys must be workspace ids");
      if (lua_type(L, -1) != LUA_TSTRING)
        return luaL_error(L, "workspace_backgrounds.set: values must be image paths");
      lua_pop(L, 1);
    }

    std::map<WORKSPACEID, std::string> wanted;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
      const WORKSPACEID id   = lua_tointeger(L, -2);
      const std::string path = lua_tostring(L, -1);
      if (!path.empty())
        wanted[id] = path;
      lua_pop(L, 1);
    }

    assign(std::move(wanted));
    return 0;
  }

  void drawSlot(PHLMONITOR monitor, const SSlot& slot, const Vector2D& logicalOffset, float alpha) {
    const auto size   = sizeOf(monitor);
    const auto offset = logicalOffset * monitor->m_scale;
    const CBox box    = CBox{std::round(offset.x), std::round(offset.y), (double)size.w, (double)size.h};

    const auto draw = [&](const SPicture& picture, float a) {
      const auto it = g_state->textures.find(SKey{picture, size});
      if (it == g_state->textures.end() || a <= 0.F)
        return;

      CTexPassElement::SRenderData data;
      data.tex = it->second;
      data.box = box;
      data.a   = a;
      g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
    };

    const float fade = slot.pending ? 0.F : slot.fade->value();
    if (fade < 1.F)
      draw(slot.previous, alpha);
    draw(slot.picture, alpha * fade);
  }

  const SSlot* slotFor(WORKSPACEID id) {
    const auto it = g_state->slots.find(id);
    return it == g_state->slots.end() ? nullptr : &it->second;
  }

  bool moving(const PHLWORKSPACE& ws) {
    return ws->m_renderOffset->isBeingAnimated() || ws->m_alpha->isBeingAnimated() || ws->m_forceRendering;
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

  // Hyprland destroys an empty workspace the moment it is left, so there is
  // nothing left to slide out, but its background still has to.
  //
  // In a keyboard switch the two workspaces animate with the same curve, the
  // one leaving from where it was to where the arriving one started, so it
  // sits behind the arriving one by exactly the distance that one started
  // from, in every slide and fade style. After a swipe both continue from
  // where the fingers left them, one swipe distance apart.
  void drawLeaving(PHLMONITOR monitor, const PHLWORKSPACE& active, SState::STrack& track) {
    if (track.leaving == WORKSPACE_INVALID)
      return;

    if (!moving(active)) {
      track.leaving = WORKSPACE_INVALID;
      return;
    }

    // Still alive, so it draws itself where Hyprland puts it.
    if (State::workspaceState()->query().id(track.leaving).run())
      return;

    const auto slot = slotFor(track.leaving);
    if (!slot)
      return;

    const auto offset = active->m_renderOffset->value();
    if (track.leavingFromSwipe) {
      // Fixed on the first frame that has one, so a spring curve overshooting
      // zero cannot flip it.
      if (track.side == Vector2D{})
        track.side = sideOf(offset);
      drawSlot(monitor, *slot, offset - track.side * swipeDistance(monitor), 1.F);
      return;
    }

    const float alpha = active->m_alpha->isBeingAnimated() ? 1.F - active->m_alpha->value() : 1.F;
    drawSlot(monitor, *slot, offset - active->m_renderOffset->begun(), alpha);
  }

  // A swipe toward a workspace that does not exist yet moves the current one
  // over empty space, and Hyprland creates the new workspace only when the
  // fingers lift. Its background is drawn where that workspace will be,
  // found the way Hyprland will find it, so it follows the fingers too, and
  // slides back out with the current workspace when the swipe is let go short.
  void drawSwipeTarget(PHLMONITOR monitor, const PHLWORKSPACE& active, SState::STrack& track, bool swiping) {
    static auto PSWIPENEW  = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_create_new");
    static auto PSWIPEUSER = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_use_r");

    const auto  offset = active->m_renderOffset->value();

    if (swiping) {
      track.swipeTarget = WORKSPACE_INVALID;
      const auto side   = sideOf(offset);
      if (side == Vector2D{})
        return;

      const bool  toRight = side.x + side.y < 0;
      WORKSPACEID target  = getWorkspaceIDNameFromString(toRight ? (*PSWIPEUSER ? "r+1" : "m+1") : (*PSWIPEUSER ? "r-1" : "m-1")).id;
      if (toRight && *PSWIPENEW && target <= active->m_id)
        target = getWorkspaceIDNameFromString("r+1").id;

      if (target == WORKSPACE_INVALID || target == active->m_id || State::workspaceState()->query().id(target).run())
        return;

      track.swipeTarget = target;
      track.swipeBegin  = active->m_id;
      track.swipeSide   = side;
    } else if (track.swipeTarget == WORKSPACE_INVALID || track.swipeBegin != active->m_id || !active->m_renderOffset->isBeingAnimated() ||
               State::workspaceState()->query().id(track.swipeTarget).run()) {
      track.swipeTarget = WORKSPACE_INVALID;
      return;
    }

    if (const auto slot = slotFor(track.swipeTarget))
      drawSlot(monitor, *slot, offset - track.swipeSide * swipeDistance(monitor), 1.F);
  }

  // A swipe let go past halfway toward a workspace that did not exist yet
  // creates it already in place, while the one left slides out over it. The
  // new background rides in beside the one leaving instead of appearing at once.
  Vector2D arrivingOffset(PHLMONITOR monitor, const PHLWORKSPACE& active, const SState::STrack& track) {
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

  void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WALLPAPER || !g_state || g_state->slots.empty())
      return;

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor || !monitor->m_activeWorkspace)
      return;

    // A monitor can change size without being added or removed again (a new
    // mode or scale), so a picture missing at this size is asked for here.
    const SSize size = sizeOf(monitor);
    for (const auto& [id, slot] : g_state->slots) {
      const SKey key{slot.picture, size};
      if (!g_state->textures.contains(key) && !g_state->failed.contains(key) && !g_state->inFlight.contains(key)) {
        requestDecodes();
        break;
      }
    }

    const auto active  = monitor->m_activeWorkspace;
    const bool swiping = active->m_forceRendering && g_pUnifiedWorkspaceSwipe->isGestureInProgress() && Desktop::focusState()->monitor() == monitor;

    auto&      track = g_state->tracks[monitor->m_id];
    if (track.active != active->m_id) {
      track.leaving          = track.active;
      track.active           = active->m_id;
      track.side             = {};
      track.leavingFromSwipe = track.swiping;
    }
    track.swiping = swiping;

    drawLeaving(monitor, active, track);
    drawSwipeTarget(monitor, active, track, swiping);

    // Workspaces on their way in or out, under the active one, so it is drawn
    // over them when the switch style overlaps them.
    for (const auto& workspace : State::workspaceState()->workspaces()) {
      const auto ws = workspace.lock();
      if (!ws || ws == active || ws->m_isSpecialWorkspace || ws->m_monitor != monitor || !moving(ws))
        continue;

      if (const auto slot = slotFor(ws->m_id))
        drawSlot(monitor, *slot, ws->m_renderOffset->value(), ws->m_alpha->value());
    }

    if (const auto slot = slotFor(active->m_id))
      drawSlot(monitor, *slot, arrivingOffset(monitor, active, track), active->m_alpha->value());
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

  // hyprctl workspacebackgrounds [-j]: the picture each workspace was given,
  // and whether it is on screen yet, so scripts read what is drawn rather than
  // work it out again.
  std::string hyprctlWorkspaceBackgrounds(eHyprCtlOutputFormat format, std::string) {
    std::string out;
    for (const auto& [id, slot] : g_state->slots) {
      const bool ready  = readyEverywhere(slot.picture);
      const bool failed = std::ranges::any_of(g_state->failed, [&](const SKey& key) { return key.picture == slot.picture; });
      if (format == eHyprCtlOutputFormat::FORMAT_JSON)
        out += std::format("{}{{\"workspace\": {}, \"path\": {}, \"ready\": {}, \"failed\": {}}}", out.empty() ? "" : ",\n", id, jsonString(slot.picture.path), ready,
                           failed);
      else
        out += std::format("{}\t{}{}\n", id, slot.picture.path, failed ? "\t(unreadable)" : ready ? "" : "\t(loading)");
    }
    return format == eHyprCtlOutputFormat::FORMAT_JSON ? "[\n" + out + "\n]\n" : out;
  }

  void onMonitorsChanged() {
    if (!g_state)
      return;
    requestDecodes();
    prune();
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

  g_state         = makeUnique<SState>();
  g_state->worker = std::thread(workerLoop);

  auto& events = Event::bus()->m_events;
  g_state->listeners.emplace_back(events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); }));
  g_state->listeners.emplace_back(events.monitor.added.listen([](PHLMONITOR) { onMonitorsChanged(); }));
  g_state->listeners.emplace_back(events.monitor.removed.listen([](PHLMONITOR) { onMonitorsChanged(); }));
  g_state->listeners.emplace_back(events.monitor.layoutChanged.listen([] { onMonitorsChanged(); }));

  HyprlandAPI::addLuaFunction(PHANDLE, "workspace_backgrounds", "set", luaSet);
  g_state->hyprctl = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{.name = "workspacebackgrounds", .exact = true, .fn = hyprctlWorkspaceBackgrounds});

  return {"workspace_backgrounds", "A background for every workspace, moving with it", "Omarchy", "0.1.0"};
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
  HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_state->hyprctl);

  for (auto& [key, job] : g_state->inFlight)
    if (job->result)
      cairo_surface_destroy(job->result);

  g_state.reset();
  damageAll();
}
