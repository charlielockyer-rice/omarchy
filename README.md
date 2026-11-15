# omarchy-hyprland-workspace-backgrounds

A native Hyprland plugin that gives every workspace a background of its own, the way every Space on a Mac has its own desktop picture. The background is drawn by Hyprland as part of its workspace, so it slides in with the workspace on a switch, follows your fingers on a swipe, and fades with the workspace in the fade styles, in the same frame as the windows on it.

A wallpaper on a layer-shell surface can't do this: it sits under every workspace at once and never learns where a workspace is mid-switch, so it can only be swapped after the switch has started, which shows as a pop or a crossfade. This plugin draws each workspace's picture right after the background layer and before the bottom layer, translated by the workspace's own render offset, which is exactly where Hyprland draws that workspace's windows.

Omarchy core has no build step, so the plugin ships as its own package, `omarchy-hyprland-workspace-backgrounds`, built by [omarchy-pkgs](https://github.com/omacom/omarchy-pkgs) from this repository's GitHub releases. Omarchy loads it from its Lua config while _Workspace Backgrounds_ is on, from a fixed path:

```lua
hl.plugin.load("/usr/lib/omarchy-hyprland-workspace-backgrounds/workspace-backgrounds.so")
```

That path is a contract with Omarchy core, as is the interface below. Which picture each workspace gets is Omarchy's policy and lives in its config (`default/hypr/workspace-backgrounds.lua`); the plugin only draws what it is given.

## Interface

```lua
hl.plugin.workspace_backgrounds.set({
  { workspace = 1, path = "/path/a.webp", monitor = "DP-1", preload = true },
  { workspace = 7, path = "/path/b.jpg",  monitor = "DP-1" },
})
```

Replaces every assignment at once. The new set is prepared out of sight and swapped in whole once its pictures are ready, never mid-slide, with the workspace on screen fading to its new picture (Hyprland's `fadeIn` animation). A workspace left out draws nothing, so the ordinary background layer shows there; an empty table hands every workspace back. `monitor` says where a workspace that does not exist yet will appear; `preload` keeps its picture on the GPU even then, so switching there is instant. Calling it again with the same set (every config reload) does nothing.

```lua
hl.plugin.workspace_backgrounds.configure({ budget_mb = 384, cache_mb = 1024, helpers = 2 })
```

All optional: the texture memory budget, the disk cache cap, how many helper processes run at once, and `cache_dir` / `helper` paths.

```bash
hyprctl workspacebackgrounds -j
```

Each workspace's picture, what it draws now, whether it failed and why, its texture level, and the textures, bytes and budget in use. The command only exists while the plugin is loaded.

## How it works

- **No decoding in Hyprland.** `workspace-backgrounds-prepare`, installed next to the plugin, is started per picture and monitor size. It caps its own memory (1 GB data), CPU time and wall time before reading anything, refuses pictures over 100 megapixels or 32768 px a side from the file header before decoding, leaves no core dump, scales the picture to cover the monitor's pixel size (rotation and scale included) and crops the middle, and writes raw pixels to `~/.cache/omarchy/workspace-backgrounds`, named by the picture's path, inode, size and mtime and the output size. The plugin only reads those files, checks their size and header exactly, and uploads them. A picture that breaks a decoder takes down only the helper.
- **What is on the GPU.** Slots marked `preload` and every workspace that exists, one texture per distinct picture and monitor size, the workspaces on screen first. Textures are opaque and drawn with blending off. Over the budget, the least urgent textures are halved, then quartered, the ones on screen last, rather than left out. An empty workspace outside those is read from the disk cache in the frame it is switched to.
- **Quiet uploads.** Uploads wait until nothing is sliding, swiping or fading; only a picture about to be on screen without its texture is uploaded at once.
- **Failure is per picture.** A picture that cannot be prepared keeps the workspace on the picture it had, at whatever size its monitor has now; it is not retried for every size.
- **Blur.** Hyprland caches a blur of the background for see-through windows and refreshes it only when a background layer changes; the plugin marks it stale whenever what it draws changes, slides included.
- **Movement.** Drawn right after the background layer at each workspace's render offset. A workspace Hyprland destroys as it is left (it was empty) has its slide copied at the moment the switch starts, so its picture finishes the same slide, also when another switch interrupts it. Swipes, including toward a workspace created on release, follow Hyprland's swipe distance.
- **Overviews.** A hook on `renderAllClientsForWorkspace` tells the plugin which workspace a frame draws, so overview plugins that draw each workspace into a tile get that workspace's picture.
- Special workspaces draw over the workspace below them as usual and get no picture of their own. Videos and animated GIFs are not supported.

## Hyprland ABI

A Hyprland plugin is only compatible with the exact Hyprland build whose headers it was compiled against, and Hyprland refuses to load a mismatched build. As with `omarchy-hyprland-titlebars`, the package in omarchy-pkgs should be rebuilt whenever `hyprland` changes, with its `hyprland=` dependency moved to the version it was rebuilt against.

Plugin-owned symbols live in an anonymous namespace and the library is built with `-fno-gnu-unique`, so it unloads and reloads cleanly. Do not use whole-library hidden visibility: Hyprland shares inline state through its headers.

## Building

With the Hyprland headers from the `hyprland` package installed (builds the plugin and its helper):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Verifying

```bash
python3 tools/verify.py build/workspace-backgrounds.so   # 21 checks, about 2 minutes
python3 tools/corpus.py build/workspace-backgrounds-prepare
python3 tools/memory.py build/workspace-backgrounds.so
```

The checks run in a fully headless Hyprland (`tools/headless.py`): its own runtime, home, state and cache directories, no seat, no parent compositor and no window anywhere, rendering on the GPU through a render node handed to Aquamarine's headless backend by a test-only preload shim. A test-only plugin (`tools/probe.cpp`) drives Hyprland's own swipe code and reads back every rendered frame, so slides and swipes are checked frame by frame: the picture's edge sits exactly at its workspace's render offset and one gap from that workspace's window in every frame. They cover keyboard slides, an emptied workspace sliding out, an interrupted switch, swipes let go short and committed, the slidefade style, a 1 to 4 jump with no blank, fade or upload, a theme switch mid-slide (swapped only after the slide, no upload in motion), a newer set cancelling an older one's helpers, see-through windows with blur on, a second monitor added and removed, a mode change keeping a failed picture's fallback, a rotated monitor, the texture budget, an overview tile, corrupt pictures, and repeated load and unload. `corpus.py` feeds the helper damaged, oversized and hostile files directly.

## License

MIT, see `LICENSE`.
