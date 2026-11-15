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
hl.plugin.workspace_backgrounds.set({ [1] = "/path/a.webp", [2] = "/path/b.jpg" })
```

Replaces every assignment at once, keyed by workspace id. A workspace left out draws nothing, so the ordinary background layer shows there; an empty table hands every workspace back. Assignments survive config reloads. A picture that changes on a workspace on screen fades in over the old one with Hyprland's `fadeIn` animation, so turning animations off makes it instant like everything else.

```bash
hyprctl workspacebackgrounds -j
```

Lists each assigned workspace, its picture, whether it is decoded yet, and whether it could not be read, so scripts read what is drawn instead of working it out again. The command only exists while the plugin is loaded, which makes it the check for whether Hyprland is drawing workspace backgrounds at all.

## Behavior

- Pictures are decoded on a worker thread, scaled to cover each monitor and cropped to it, so the GPU holds exactly one screen of pixels per distinct picture and monitor size, and a picture of another shape never draws past its workspace's edge mid-slide. Every assigned picture is decoded ahead of time, so a switch to a workspace that does not exist yet still slides its picture in.
- Hyprland destroys an empty workspace the moment it is left. Its picture still leaves with it: in a keyboard switch both workspaces animate on the same curve, so it sits behind the arriving one by the distance that one started from, faded to match in the fade styles; after a swipe it stays one monitor plus `general:gaps_workspaces` away, as Hyprland keeps swiped workspaces.
- A swipe toward a workspace that does not exist yet (`gestures:workspace_swipe_create_new`) draws that workspace's picture where Hyprland will create it when the fingers lift, found the way Hyprland finds it. Let go short, it slides back out with the current workspace; let go past halfway, Hyprland creates the workspace already in place, and its picture rides in beside the one leaving rather than appearing at once.
- A picture is identified by its file as it is now, so a theme switch that puts a different picture at the same path is decoded again.
- A file that cannot be read keeps the picture the workspace had and logs the error.
- Special workspaces draw over the workspace below them as usual and get no picture of their own.

## Hyprland ABI

A Hyprland plugin is only compatible with the exact Hyprland build whose headers it was compiled against, and Hyprland refuses to load a mismatched build. As with `omarchy-hyprland-titlebars`, the package in omarchy-pkgs should be rebuilt whenever `hyprland` changes, with its `hyprland=` dependency moved to the version it was rebuilt against.

Plugin-owned symbols live in an anonymous namespace and the library is built with `-fno-gnu-unique`, so it unloads and reloads cleanly. Do not use whole-library hidden visibility: Hyprland shares inline state through its headers.

## Building

With the Hyprland headers from the `hyprland` package installed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Verifying

```bash
python3 tools/verify.py build/workspace-backgrounds.so
python3 tools/verify.py build/workspace-backgrounds.so --scale 2
```

The verifier needs a running Hyprland session with exactly one Hyprland instance, plus `foot`, `grim`, `magick`, `g++` and `pkg-config`. It starts a disposable nested Hyprland with its own runtime directory and never loads the candidate library into the parent compositor. Every check reads real frames back:

- repeated load, assign, config reload and unload, and assignments surviving a reload
- a keyboard switch: in every captured frame the picture's edge and the window on that workspace stay exactly one gap apart while both move
- leaving an empty workspace that Hyprland destroys, whose picture must still slide out
- a swipe, through Hyprland's own workspace swipe code (`tools/swipe.cpp`, a test-only plugin, drives it because a nested compositor gets no touchpad): the pictures track the gesture, keep tracking the window while the release animates, both when let go short and when it commits into a workspace created on release, and a slow short swipe settles back
- the `slidefade` style, where the picture of an empty workspace being left fades out in place
- a workspace without a picture showing the background layer, a picture fading in over the old one, a wider picture cropped without spilling into the next workspace, a new picture at the same path, an unreadable file, and clearing and unloading

Screenshots go to `~/.cache/omarchy-hyprland-workspace-backgrounds-verification` by default; pass `--artifacts DIR` to change it.

## License

MIT, see `LICENSE`.
