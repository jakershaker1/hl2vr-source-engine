# HL2VR — Android XR build

Standalone VR build of the engine for Android XR headsets (developed against a
Samsung Galaxy XR). Renders stereo through OpenXR with an OpenGL ES graphics
binding, using the real `shaderapidx9` renderer via `togles`.

## Building

Configure (from the repo root):

```bash
./waf configure -T release --android=aarch64,clang,28 --disable-warns --togles --use-togl=1 --use-sdl=1
```

```bash
./waf build
```

Then package and install:

```bash
cd android && gradle assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### `-T release` is not optional

**Use `-T release`, not `-T debug`.** The build type is a configure-time flag
and lives in `build/` (gitignored), so nothing in the tree will remind you and
nothing will look wrong — the debug build runs correctly, just far slower.

Measured on a Galaxy XR, same scene, native per-eye resolution:

| build      | frame time      |
| ---------- | --------------- |
| `-T debug` | 116.6 ms (~9 fps)  |
| `-T release` | 76.0 ms (~13 fps) |

`-T debug` compiles the whole engine `-O0`. This cost roughly a full evening of
performance investigation before anyone thought to check the compiler flags, so
if the game is inexplicably slow, check this first.

## Runtime debug properties

All off by default; read once at startup, so set them before launching.

```bash
# Per-frame timing breakdown (engine vs xrWaitFrame vs blit vs xrEndFrame),
# averaged over 100 frames. This is the only reliable view of where a frame
# goes - the system performance HUD's "App CPU/GPU time" counters do not
# account for the whole frame and disagree with wall clock by 4-5x.
adb shell setprop debug.hl2vr.frametime 1

# Render scale, applied to the runtime's recommended per-eye size.
# NOTE: measured to make no difference to frame time on this device - the
# bottleneck is per-scene-traversal CPU (draw calls through togles, paid twice
# for stereo), not fill rate. Kept as a quality/VRAM dial, not a perf fix.
adb shell setprop debug.hl2vr.resscale 0.7
```

## Content and assets

Game content is read from the app's external files dir, since the engine's raw
POSIX file I/O cannot reach arbitrary `/sdcard` paths under scoped storage:

```
/sdcard/Android/data/org.hl2vr.game/files/hl2vr_content/{hl2,platform}
```

### Fonts

Fonts come from the game content you pushed, at
`<content>/platform/resource/linux_fonts/` — the same set the desktop Linux
build uses. Nothing extra to install, and nothing font-related is bundled in
this repo.

Upstream's Android font path looked in `$APP_DATA_PATH/files/` instead, which
is not where this port keeps content, so every lookup missed. It also asked for
`LiberationMono-Regular.ttf` while the shipped file is
`liberationmono-regular.ttf` — Android's filesystem is case-sensitive, so that
one could never resolve either way. Both are handled in
`AndroidResolveFontPath` (`vgui2/vgui_surfacelib/linuxfont.cpp`), which prefers
the content directory and falls back to the legacy location.

Symptom if this ever regresses: `Failed to load custom font file` in logcat and
**no menu or HUD text at all**, while the 3D world still renders normally — it
looks like a UI or stereo bug rather than a missing asset.
