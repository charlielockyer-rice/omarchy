-- A background for every workspace, the way every Space on a Mac has its own
-- desktop picture: it slides in with the workspace when you switch and follows
-- your fingers when you swipe.
--
-- A wallpaper on a layer sits under every workspace at once, so it can only be
-- swapped once a switch is under way. The omarchy-hyprland-workspace-backgrounds
-- plugin draws each picture inside Hyprland as part of its workspace instead.
-- This file decides which picture each workspace gets and hands the plugin the
-- whole list; the shell's background layer stays underneath, showing through
-- wherever a workspace has no picture of its own.
--
-- Workspace N shows the theme's Nth background, wrapping around, on every
-- monitor's own workspaces 1-10. Picking a background while the toggle is on
-- gives it to the focused workspace only; those picks are kept per theme in
-- ~/.local/state/omarchy/workspace-backgrounds/<theme>, so a theme comes back
-- the way it was left. Videos stay on the background layer, so a workspace
-- given one shows the shell's background there.

local paths = require("default.hypr.paths")
local workspaces = require("default.hypr.monitor-workspaces")

local CURRENT = paths.state_home .. "/omarchy/current"
local PICKS = paths.state_home .. "/omarchy/workspace-backgrounds"

-- What the plugin decodes. GIFs and videos are left to the shell.
local STILLS = { jpg = true, jpeg = true, png = true, webp = true, bmp = true, jxl = true, avif = true }

local M = {
  -- Where the omarchy-hyprland-workspace-backgrounds package puts the plugin.
  plugin = "/usr/lib/omarchy-hyprland-workspace-backgrounds/workspace-backgrounds.so",
}

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function read_line(path)
  local file = io.open(path, "r")
  if not file then
    return nil
  end

  local line = file:read("*l")
  file:close()
  return line
end

local function exists(path)
  local file = io.open(path, "r")
  if file then
    file:close()
    return true
  end

  return false
end

local function still(path)
  local extension = path:match("%.([^./]+)$")
  return extension ~= nil and STILLS[extension:lower()] == true
end

-- The theme's backgrounds in the order `omarchy theme bg next` cycles them:
-- the user's own for this theme, then the theme's.
local function backgrounds(theme)
  local dirs = shell_quote(paths.config_home .. "/omarchy/backgrounds/" .. theme .. "/") .. " " .. shell_quote(CURRENT .. "/theme/backgrounds/")
  local pipe = io.popen("find -L " .. dirs .. " -maxdepth 1 -type f 2>/dev/null | sort")
  local list = {}
  if pipe then
    for path in pipe:lines() do
      if still(path) then
        list[#list + 1] = path
      end
    end
    pipe:close()
  end

  return list
end

function M.picks_file(theme)
  return PICKS .. "/" .. theme
end

-- Workspace id -> picked path, for the background picker's choices.
local function picks(theme)
  local chosen = {}
  local file = io.open(M.picks_file(theme), "r")
  if not file then
    return chosen
  end

  for line in file:lines() do
    local id, path = line:match("^(%d+)\t(.+)$")
    if id then
      chosen[tonumber(id)] = path
    end
  end

  file:close()
  return chosen
end

-- Workspace id -> picture, for every workspace of every connected monitor.
function M.assignments()
  local theme = read_line(CURRENT .. "/theme.name")
  if not theme or theme == "" then
    return {}
  end

  local list = backgrounds(theme)
  local chosen = picks(theme)
  local map = {}

  for _, monitor in ipairs(hl.get_monitors()) do
    if not monitor.is_mirror then
      for slot = 1, workspaces.slots do
        local id = workspaces.workspace_id(monitor, slot)
        if #list > 0 then
          map[id] = list[(slot - 1) % #list + 1]
        end
      end
    end
  end

  -- A pick whose file has gone falls back to the default.
  for id, path in pairs(chosen) do
    if exists(path) then
      map[id] = still(path) and path or nil
    end
  end

  return map
end

function M.apply()
  if not hl.plugin.workspace_backgrounds then
    return
  end

  hl.plugin.workspace_backgrounds.set(M.assignments())
end

function M.enable()
  if not exists(M.plugin) then
    return
  end

  hl.plugin.load(M.plugin)
  M.apply()

  -- A new monitor gets its block of workspaces from monitor-workspaces.lua,
  -- whose handlers were registered first and so run first.
  hl.on("hyprland.start", M.apply)
  hl.on("monitor.added", M.apply)
end

return M
