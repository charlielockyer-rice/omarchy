// Test-only plugin for tools/verify.py. Never part of the package.
//
// - Drives Hyprland's own workspace swipe, the code a touchpad swipe runs,
//   because a headless compositor has no touchpad.
// - Records every frame it renders: chosen rows of the finished frame, read
//   back from the GPU just before it is presented, with the active
//   workspace's render offset at that frame. The verifier checks every frame
//   of a slide or swipe this way, not a few sampled screenshots.
// - Draws another workspace into a tile of the frame through
//   renderWorkspace, the way overview plugins do.
#define WLR_USE_UNSTABLE

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
// renderWorkspace is what overview plugins call; it is protected, so this
// test-only plugin opens it up the way those plugins do.
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <GLES3/gl32.h>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {
  HANDLE           PHANDLE = nullptr;
  FILE*            g_out   = nullptr;
  std::vector<int> g_rows;
  uint64_t         g_frame = 0;
  WORKSPACEID      g_tile  = WORKSPACE_INVALID;
  CBox             g_tileBox;
  std::vector<CHyprSignalListener> g_listeners;
  // Whether Hyprland's cached blur of the background was stale going into
  // the frame (so it is redrawn this frame).
  bool             g_blurDirty = false;

  const char       B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string      base64(const std::vector<uint8_t>& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
      uint32_t v = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
      out += B64[v >> 18 & 63];
      out += B64[v >> 12 & 63];
      out += i + 1 < in.size() ? B64[v >> 6 & 63] : '=';
      out += i + 2 < in.size() ? B64[v & 63] : '=';
    }
    return out;
  }

  class CReadback : public IPassElement {
    public:
    std::vector<UP<IPassElement>> draw() override {
      const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
      if (!monitor || !g_out)
        return {};
      const int  w = monitor->m_transformedSize.x, h = monitor->m_transformedSize.y;
      const auto ws = monitor->m_activeWorkspace;
      std::fprintf(g_out, "{\"frame\": %llu, \"monitor\": \"%s\", \"width\": %d, \"height\": %d, \"active\": %lld, \"offset\": [%.3f, %.3f], \"alpha\": %.4f, \"blur_dirty\": %s, \"rows\": {",
                   (unsigned long long)g_frame++, monitor->m_name.c_str(), w, h, ws ? (long long)ws->m_id : -1LL, ws ? ws->m_renderOffset->value().x : 0.0,
                   ws ? ws->m_renderOffset->value().y : 0.0, ws ? ws->m_alpha->value() : 1.0, g_blurDirty ? "true" : "false");
      std::vector<uint8_t> rgba(w * 4), rgb(w * 3);
      bool                 first = true;
      // Read what this frame drew: Hyprland binds only the draw framebuffer,
      // and the read binding can still name an older one.
      GLint drawFB = 0, readFB = 0;
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFB);
      for (const int y : g_rows) {
        if (y < 0 || y >= h)
          continue;
        // GL's rows count from the bottom.
        glReadPixels(0, h - 1 - y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (int x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c)
            rgb[x * 3 + c] = rgba[x * 4 + c];
        std::fprintf(g_out, "%s\"%d\": \"%s\"", first ? "" : ", ", y, base64(rgb).c_str());
        first = false;
      }
      glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
      std::fprintf(g_out, "}}\n");
      std::fflush(g_out);
      return {};
    }
    bool needsLiveBlur() override {
      return false;
    }
    bool needsPrecomputeBlur() override {
      return false;
    }
    const char* passName() override {
      return "CProbeReadback";
    }
    ePassElementType type() override {
      return EK_CUSTOM;
    }
    bool undiscardable() override {
      return true;
    }
  };

  void onStage(eRenderStage stage) {
    if (stage == RENDER_POST_WINDOWS && g_tile != WORKSPACE_INVALID) {
      const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
      const auto ws      = State::workspaceState()->query().id(g_tile).run();
      static bool inside = false;
      if (monitor && ws && !inside) {
        inside = true;
        g_pHyprRenderer->renderWorkspace(monitor, ws, Time::steadyNow(), g_tileBox);
        inside = false;
      }
    }
    if (stage == RENDER_LAST_MOMENT && g_out)
      g_pHyprRenderer->m_renderPass.add(makeUnique<CReadback>());
  }

  int luaBegin(lua_State*) {
    g_pUnifiedWorkspaceSwipe->begin();
    return 0;
  }
  int luaUpdate(lua_State* L) {
    g_pUnifiedWorkspaceSwipe->update(luaL_checknumber(L, 1));
    return 0;
  }
  int luaFinish(lua_State*) {
    g_pUnifiedWorkspaceSwipe->end();
    return 0;
  }

  // probe.record("/path/frames.jsonl", { 10, 450 }) / probe.record()
  int luaRecord(lua_State* L) {
    if (g_out) {
      std::fclose(g_out);
      g_out = nullptr;
    }
    if (lua_gettop(L) == 0)
      return 0;
    const char* path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    g_rows.clear();
    for (int i = 1; i <= (int)lua_rawlen(L, 2); ++i) {
      lua_rawgeti(L, 2, i);
      g_rows.push_back((int)lua_tointeger(L, -1));
      lua_pop(L, 1);
    }
    g_out = std::fopen(path, "w");
    return 0;
  }

  // probe.tile(workspace, x, y, w, h) draws that workspace into a tile; probe.tile() stops.
  int luaTile(lua_State* L) {
    if (lua_gettop(L) == 0) {
      g_tile = WORKSPACE_INVALID;
      return 0;
    }
    g_tile    = luaL_checkinteger(L, 1);
    g_tileBox = CBox{luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4), luaL_checknumber(L, 5)};
    return 0;
  }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
  return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;
  HyprlandAPI::addLuaFunction(handle, "probe", "swipe_begin", luaBegin);
  HyprlandAPI::addLuaFunction(handle, "probe", "swipe_update", luaUpdate);
  HyprlandAPI::addLuaFunction(handle, "probe", "swipe_finish", luaFinish);
  HyprlandAPI::addLuaFunction(handle, "probe", "record", luaRecord);
  HyprlandAPI::addLuaFunction(handle, "probe", "tile", luaTile);
  g_listeners.emplace_back(Event::bus()->m_events.render.stage.listen([](eRenderStage s) { onStage(s); }));
  g_listeners.emplace_back(Event::bus()->m_events.render.pre.listen([](PHLMONITOR m) { g_blurDirty = m && m->m_blurFBDirty; }));
  return {"probe", "Test probe for workspace backgrounds", "Omarchy", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  if (g_out)
    std::fclose(g_out);
  g_out = nullptr;
  g_listeners.clear();
  g_pHyprRenderer->m_renderPass.removeAllOfType("CProbeReadback");
}
