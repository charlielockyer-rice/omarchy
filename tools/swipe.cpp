// Test-only plugin for tools/verify.py: drives Hyprland's own workspace swipe,
// the code a three-finger touchpad swipe runs, from Lua, because a nested
// compositor gets no touchpad. Never part of the package.
#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {
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
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
  return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  HyprlandAPI::addLuaFunction(handle, "swipe_driver", "begin", luaBegin);
  HyprlandAPI::addLuaFunction(handle, "swipe_driver", "update", luaUpdate);
  HyprlandAPI::addLuaFunction(handle, "swipe_driver", "finish", luaFinish);
  return {"swipe_driver", "Drives the workspace swipe for tests", "Omarchy", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {}
