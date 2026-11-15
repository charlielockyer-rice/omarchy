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
-- and a mirror, which has no workspaces of its own. A third, unplugged, owns
-- block 2; its workspace 23 is a guest on the laptop, and 4 is a workspace
-- of the unplugged one that was merged into the laptop's block.
local blocks = { eDP = 0, HDMI = 1 }
local identity_of = { [0] = "Laptop", [1] = "Dell U2723QE", [2] = "LG 5K" }
package.preload["default.hypr.monitor-workspaces"] = function()
  return {
    slots = 10,
    workspace_id = function(monitor, slot) return blocks[monitor.name] * 10 + slot end,
    owner = function(id)
      if id == 4 and os.getenv("MERGED") then return "LG 5K", 7 end
      return identity_of[(id - 1) // 10], (id - 1) % 10 + 1
    end,
  }
end

local handed
local active = tonumber(os.getenv("ACTIVE") or "1")
hl = {
  get_monitors = function()
    return { { name = "eDP" }, { name = "HDMI" }, { name = "DP", is_mirror = true } }
  end,
  get_workspaces = function()
    return { { id = 1 }, { id = 13 }, { id = 23 }, { id = -98, special = true } }
  end,
  get_active_workspace = function() return { id = active } end,
  plugin = { workspace_backgrounds = { set = function(entries) handed = entries end } },
}

local backgrounds = require("default.hypr.workspace-backgrounds")
if os.getenv("PICK") then
  print(tostring(backgrounds.pick(os.getenv("PICK"))))
end
backgrounds.apply()

table.sort(handed, function(a, b) return a.workspace < b.workspace end)
for _, e in ipairs(handed) do
  print(e.workspace .. " " .. e.path:match("[^/]+$") .. " " .. (e.monitor or "-") .. " " .. tostring(e.preload == true))
end
LUA

run() {
  ROOT="$ROOT" XDG_STATE_HOME="$state" XDG_CONFIG_HOME="$config" lua "$tmpdir/run.lua"
}

ids() {
  awk '{ print $1 " " $2 }'
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
[[ $(head -n 10 <<<"$output" | ids) == "$expected_cycle" ]] || fail "workspace N shows the Nth still, the user's own first, wrapping around" "$output"
[[ $(sed -n 11p <<<"$output" | ids) == "11 0-mine.png" && $(sed -n 20p <<<"$output" | ids) == "20 1-hills.jpg" ]] ||
  fail "every monitor's own workspaces 1-10 start the cycle over, and a mirror adds none" "$output"
pass "workspace N shows the theme's Nth still on every monitor, skipping videos"

grep -qx '1 0-mine.png eDP true' <<<"$output" && grep -qx '5 0-mine.png eDP true' <<<"$output" && grep -qx '6 1-hills.jpg eDP false' <<<"$output" ||
  fail "slots 1-5 of each monitor are preloaded and placed on their monitor" "$output"
grep -qx '23 2-sea.webp - false' <<<"$output" && [[ $(wc -l <<<"$output") == 21 ]] ||
  fail "a workspace whose monitor is unplugged keeps its slot's picture where it is now" "$output"
pass "the plugin is told each workspace's monitor and which to keep ready"

ACTIVE=13 PICK="$state/omarchy/current/theme/backgrounds/4-city.png" run >/dev/null
grep -qx $'Dell U2723QE\t3\t'"$state/omarchy/current/theme/backgrounds/4-city.png" "$state/omarchy/workspace-backgrounds/dusk" ||
  fail "a pick is kept by monitor and slot" "$(cat "$state/omarchy/workspace-backgrounds/dusk")"
output=$(run)
grep -qx '13 4-city.png' < <(ids <<<"$output") || fail "a picked background replaces the workspace's default" "$output"
grep -qx '3 2-sea.webp' < <(ids <<<"$output") || fail "the same slot on another monitor keeps its own" "$output"
blocks_swapped=$(sed 's/blocks\[monitor.name\]/(monitor.name == "eDP" and 1 or 0)/; s/identity_of\[(id - 1) \/\/ 10\]/({ [0] = "Dell U2723QE", [1] = "Laptop" })[(id - 1) \/\/ 10]/' "$tmpdir/run.lua")
printf '%s\n' "$blocks_swapped" >"$tmpdir/run-swapped.lua"
output=$(ROOT="$ROOT" XDG_STATE_HOME="$state" XDG_CONFIG_HOME="$config" lua "$tmpdir/run-swapped.lua")
grep -qx '3 4-city.png' < <(ids <<<"$output") || fail "a pick follows its monitor when its workspaces are renumbered" "$output"
pass "picks belong to the monitor and slot, not the workspace number"

printf '%s\t%s\t%s\n' "Laptop" 2 "$state/omarchy/current/theme/backgrounds/3-loop.mp4" "Laptop" 4 "$tmpdir/gone.png" "LG 5K" 7 "$config/omarchy/backgrounds/dusk/0-mine.png" >>"$state/omarchy/workspace-backgrounds/dusk"
output=$(MERGED=1 run)
grep -qx '2 1-hills.jpg' < <(ids <<<"$output") || fail "a video can't be a workspace's own, so its default stays" "$output"
grep -qx '4 0-mine.png' < <(ids <<<"$output") || fail "a workspace merged in from an unplugged monitor keeps that monitor's pick" "$output"
output=$(run)
grep -qx '4 4-city.png' < <(ids <<<"$output") || fail "a pick whose file is gone falls back to the default" "$output"
[[ $(ACTIVE=2 PICK="$state/omarchy/current/theme/backgrounds/3-loop.mp4" run | head -n 1) == "false" ]] || fail "picking a video for one workspace is refused"
pass "videos stay out, and missing picks fall back"

ACTIVE=11 PICK="$state/omarchy/current/theme/backgrounds/4-city.png" run >/dev/null
printf 'dawn\n' >"$state/omarchy/current/theme.name"
output=$(run)
# dawn has no backgrounds of the user's own, so its first is the theme's first.
grep -qx '11 1-hills.jpg' < <(ids <<<"$output") || fail "picks belong to the theme they were made in" "$output"
pass "picks are kept per theme"
