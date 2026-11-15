# Backgrounds

Every theme ships with its own set of backgrounds, and you can add extras of your own in `~/.config/omarchy/backgrounds/[theme]`. If you want to add an extra background image to, say, the nord theme, you just put the file in `~/.config/omarchy/backgrounds/nord`.

You can do this most easily by going to _Install > Style > Background_ in the Omarchy Menu. That'll bring up the folder where the backgrounds for that theme is stored. Hit `Super + Shift + F` to start another file manager, find your background, copy it over.  Now it'll be included in the choices of backgrounds you can select between using `Super + Ctrl + W`.

Backgrounds can be videos as well as stills. Drop an `mp4`, `m4v`, `mov`, `webm`, `mkv`, or `avi` file in the same folder and it appears alongside the images. Videos are played by the OWE wallpaper engine. It decodes the video once for all monitors and plays its sound through the default audio output, and it stops playback whenever nothing can see it. A video wallpaper still costs far more power than a still one.

Themes can include a short intro for a still background. For `backgrounds/road.webp`, the matching video lives at `backgrounds/intros/road.mp4`. It plays automatically at boot, when switching to a theme with that background selected, and when choosing that background, then returns to the still. Intro videos stay out of the background picker and never loop.

Turning off _Appearance > Animations_ also turns off background intros. Omarchy does this by default in a virtual machine. The `omarchy theme bg intro toggle` command can toggle intros separately. Refreshing a theme does not play an intro.

## A background for every workspace

Turn on _Appearance > Workspace Backgrounds On_ (or run `omarchy toggle workspace-backgrounds`) and every workspace gets a background of its own, the way every Space on a Mac has its own desktop picture. Hyprland draws it as part of the workspace, so it changes in the same frame as the windows when you switch. If you turn on Hyprland's workspace slide animation, or a swipe gesture that follows your fingers, the background slides along with the workspace.

Workspace 1 shows the theme's first background, workspace 2 its second, and so on, starting over when the theme runs out. Each monitor's own workspaces 1-10 follow the same order. With it on, picking a background with `Super + Ctrl + W` or cycling with `omarchy theme bg next` changes only the workspace you're on. Those picks are remembered per theme, so switching back to a theme brings its workspaces back the way you left them.

Stills are drawn on each workspace. A workspace given a video shows it behind the windows, but it stays put while you switch rather than sliding with the workspace, and background intros don't play while workspace backgrounds are on. _Appearance > Workspace Backgrounds Off_ goes back to one background everywhere.

You can find a huge collection of cool curated backgrounds in [dharmx/walls](https://github.com/dharmx/walls).

The lock screen shows a still frame of a video background, the same way it shows a still frame of an animated GIF.
