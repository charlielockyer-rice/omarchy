-- A background for every workspace, the way every Space on a Mac has its own
-- desktop picture: it slides in with the workspace when you switch and follows
-- your fingers when you swipe.
--
-- A wallpaper on a layer sits under every workspace at once, so it can only be
-- swapped once a switch is under way. The omarchy-hyprland-workspace-backgrounds
-- plugin draws each picture inside Hyprland as part of its workspace instead,
-- from pixels its own helper process prepares (no image is decoded inside
-- Hyprland). This file decides which picture each workspace gets and hands the
-- plugin the whole list; the shell's background layer stays underneath,
-- showing through wherever a workspace has no picture of its own.
--
-- Workspace N shows the theme's Nth background, wrapping around, on every
-- monitor's own workspaces 1-10. Picking a background while the toggle is on
-- gives it to the focused workspace only. A pick belongs to the physical
-- monitor and the slot (its make, model and serial, and 1-10), not to the
-- workspace's number, so it survives unplugging and Omarchy renumbering
-- workspaces. Picks are kept per theme in
-- ~/.local/state/omarchy/workspace-backgrounds/<theme>, so a theme comes back
-- the way it was left.
--
-- Stills only: videos and animated GIFs stay one background for every
-- workspace, so with this on they can't be picked for one workspace. The lock
-- screen keeps showing the theme's background.

local paths = require("default.hypr.paths")
local workspaces = require("default.hypr.monitor-workspaces")

local CURRENT = paths.state_home .. "/omarchy/current"
local PICKS = paths.state_home .. "/omarchy/workspace-backgrounds"

-- What the plugin's helper decodes.
local STILLS = { jpg = true, jpeg = true, png = true, webp = true, bmp = true, jxl = true }

-- Slots kept ready on the GPU on every monitor, so switching to them is
-- instant even before they exist. Other workspaces are kept ready while they
-- exist (have windows); an empty one past these loads as it is switched to.
local PRELOADED = 5

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

function M.still(path)
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
      if M.still(path) then
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

-- identity -> slot -> picked path. One line per pick: identity, slot, path.
local function picks(theme)
  local chosen = {}
  local file = io.open(M.picks_file(theme), "r")
  if not file then
    return chosen
  end

  for line in file:lines() do
    local identity, slot, path = line:match("^([^\t]+)\t(%d+)\t(.+)$")
    if identity then
      chosen[identity] = chosen[identity] or {}
      chosen[identity][tonumber(slot)] = path
    end
  end

  file:close()
  return chosen
end

local function theme_name()
  local theme = read_line(CURRENT .. "/theme.name")
  if not theme or theme == "" then
    return nil
  end
  return theme
end

-- Everything the plugin is given: an entry per workspace of every connected
-- monitor, and per workspace that exists away from its own monitor (one whose
-- monitor is unplugged).
function M.assignments()
  local theme = theme_name()
  if not theme then
    return {}
  end

  local list = backgrounds(theme)
  local chosen = picks(theme)
  local entries = {}
  local seen = {}

  local function picture(identity, slot)
    local pick = identity and chosen[identity] and chosen[identity][slot]
    -- A pick whose file has gone falls back to the default.
    if pick and exists(pick) and M.still(pick) then
      return pick
    end
    if #list > 0 then
      return list[(slot - 1) % #list + 1]
    end
    return nil
  end

  for _, monitor in ipairs(hl.get_monitors()) do
    if not monitor.is_mirror then
      for slot = 1, workspaces.slots do
        local id = workspaces.workspace_id(monitor, slot)
        local path = picture(workspaces.owner(id))
        if path then
          entries[#entries + 1] = { workspace = id, path = path, monitor = monitor.name, preload = slot <= PRELOADED }
          seen[id] = true
        end
      end
    end
  end

  for _, workspace in ipairs(hl.get_workspaces and hl.get_workspaces() or {}) do
    if not workspace.special and workspace.id > 0 and not seen[workspace.id] then
      local identity, slot = workspaces.owner(workspace.id)
      local path = slot and picture(identity, slot)
      if path then
        entries[#entries + 1] = { workspace = workspace.id, path = path }
        seen[workspace.id] = true
      end
    end
  end

  return entries
end

function M.apply()
  if not hl.plugin.workspace_backgrounds then
    return
  end

  hl.plugin.workspace_backgrounds.set(M.assignments())
end

-- Gives the focused workspace its own background, in the current theme.
-- Returns false for anything that is not a still.
function M.pick(path)
  local theme = theme_name()
  local workspace = hl.get_active_workspace()
  if not theme or not workspace or not M.still(path) then
    return false
  end

  local identity, slot = workspaces.owner(workspace.id)
  if not identity then
    return false
  end

  local lines = {}
  local file = io.open(M.picks_file(theme), "r")
  if file then
    for line in file:lines() do
      local id, s = line:match("^([^\t]+)\t(%d+)\t")
      if not (id == identity and tonumber(s) == slot) then
        lines[#lines + 1] = line
      end
    end
    file:close()
  end
  lines[#lines + 1] = identity .. "\t" .. slot .. "\t" .. path

  os.execute("mkdir -p " .. shell_quote(PICKS))
  local temp = M.picks_file(theme) .. ".tmp"
  local out = io.open(temp, "w")
  if not out then
    return false
  end
  out:write(table.concat(lines, "\n") .. "\n")
  out:close()
  os.rename(temp, M.picks_file(theme))

  M.apply()
  return true
end

function M.enable()
  if not exists(M.plugin) then
    return
  end

  hl.plugin.load(M.plugin)
  M.apply()

  -- A new monitor gets its block of workspaces from monitor-workspaces.lua,
  -- whose handlers were registered first and so run first; an unplugged one's
  -- workspaces move to another monitor and keep their pictures there.
  hl.on("hyprland.start", M.apply)
  hl.on("monitor.added", M.apply)
  hl.on("monitor.removed", M.apply)
end

return M
