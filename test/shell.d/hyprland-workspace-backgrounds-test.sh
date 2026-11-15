#!/bin/bash

# Every workspace gets a background of its own. Runs
# default/hypr/workspace-backgrounds.lua against a theme on disk and records
# what it hands the plugin.

set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/base-test.sh"

require_command lua

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state="$tmpdir/state"
config="$tmpdir/config"
mkdir -p "$state/omarchy/current/theme/backgrounds" "$config/omarchy/backgrounds/dusk" "$state/omarchy/workspace-backgrounds"
printf 'dusk\n' >"$state/omarchy/current/theme.name"
for name in 1-hills.jpg 2-sea.webp 3-loop.mp4 4-city.png; do
  : >"$state/omarchy/current/theme/backgrounds/$name"
done
: >"$config/omarchy/backgrounds/dusk/0-mine.png"

cat >"$tmpdir/run.lua" <<'LUA'
package.path = os.getenv("ROOT") .. "/?.lua;" .. package.path

-- Two monitors: the laptop panel with block 0, an external one with block 1,
-- and a mirror, which has no workspaces of its own.
local blocks = { eDP = 0, HDMI = 1 }
package.preload["default.hypr.monitor-workspaces"] = function()
  return { slots = 10, workspace_id = function(monitor, slot) return blocks[monitor.name] * 10 + slot end }
end

local handed
hl = {
  get_monitors = function()
    return { { name = "eDP" }, { name = "HDMI" }, { name = "DP", is_mirror = true } }
  end,
  plugin = { workspace_backgrounds = { set = function(map) handed = map end } },
}

local backgrounds = require("default.hypr.workspace-backgrounds")
backgrounds.apply()

local ids = {}
for id in pairs(handed) do ids[#ids + 1] = id end
table.sort(ids)
for _, id in ipairs(ids) do
  print(id .. " " .. handed[id]:match("[^/]+$"))
end
LUA

run() {
  ROOT="$ROOT" XDG_STATE_HOME="$state" XDG_CONFIG_HOME="$config" lua "$tmpdir/run.lua"
}

expected_cycle="1 0-mine.png
2 1-hills.jpg
3 2-sea.webp
4 4-city.png
5 0-mine.png
6 1-hills.jpg
7 2-sea.webp
8 4-city.png
9 0-mine.png
10 1-hills.jpg"

output=$(run)
[[ $(head -n 10 <<<"$output") == "$expected_cycle" ]] || fail "workspace N shows the Nth still, the user's own first, wrapping around" "$output"
[[ $(sed -n 11p <<<"$output") == "11 0-mine.png" && $(sed -n 20p <<<"$output") == "20 1-hills.jpg" && $(wc -l <<<"$output") == 20 ]] ||
  fail "every monitor's own workspaces 1-10 start the cycle over, and a mirror adds none" "$output"
pass "workspace N shows the theme's Nth still on every monitor, skipping videos"

printf '%s\t%s\n' 2 "$state/omarchy/current/theme/backgrounds/4-city.png" 13 "$state/omarchy/current/theme/backgrounds/3-loop.mp4" 14 "$tmpdir/gone.png" >"$state/omarchy/workspace-backgrounds/dusk"
output=$(run)
grep -qx '2 4-city.png' <<<"$output" || fail "a picked background replaces the workspace's default" "$output"
! grep -q '^13 ' <<<"$output" || fail "a workspace given a video is left to the background layer" "$output"
grep -qx '14 4-city.png' <<<"$output" || fail "a pick whose file is gone falls back to the default" "$output"
pass "picks override the defaults per workspace, leaving videos to the background layer"

printf 'dawn\n' >"$state/omarchy/current/theme.name"
output=$(run)
# dawn has no backgrounds of the user's own, so its second is the theme's second.
grep -qx '2 2-sea.webp' <<<"$output" || fail "picks belong to the theme they were made in" "$output"
pass "picks are kept per theme"
