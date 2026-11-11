# MyOBS

**English** · [Русский](README.ru.md)

**A live production switcher for Windows: vMix-style switching, broadcast-hardware reliability.**

> Status: **in development** (stage 1 of 5). The images below are **interface concepts**, not
> screenshots of the finished program. Builds appear under Actions / Releases.

![Vertical 9:16 project](docs/concept/vertical.png)

**MyOBS** is an open-source live production switcher for Windows in the spirit of vMix, built on the
OBS engine (libobs) used as a library. The interface, switcher, clip playback, audio and triggers are
its own.

**Why.** OBS is great for a single streamer, but a multi-camera show with clips, calls and LED screens
runs into its design. We measured it: on every take of a regular video clip OBS puts a blank frame on
air. MyOBS is built differently:

- **The program output comes first.** Thumbnails, multiview and screens get whatever is left of the frame budget.
- **Clips like in an editor.** On every output tick a clip hands over the frame *by number* from a
  pre-decoded buffer, so a blank or foreign frame cannot happen. Pause means the number stops growing:
  a clip resumes exactly where it left the air (vMix behaviour).
- **Frame-accurate switching.** CUT, fade and Merge happen on a frame boundary; a click during a transition is queued, not lost.
- **Standard working codec: H.264** (key frame every 10 frames, no B-frames) — instant seeking, NVIDIA hardware decoding, no third-party codecs.
- **Audio on the same frame clock** as video, so it never drifts after pauses or seeks.
- **Discrete GPU only** for rendering, even when the main monitor is on the integrated one.

**Planned:** Preview/Program with CUT, Fade, Merge and stingers; inputs (clip, camera, DeckLink, NDI,
image, browser, WebRTC calls, compositions — each input is a layered scene); vMix-style tiles with live
thumbnails, audio meter/fader, overlay buttons and record; colour-coded tabs; audio buses Master/A/B,
PFL, audio-follows-video, noise suppression, gate and echo cancellation for calls; 4 overlay channels;
any source to any monitor or LED wall; vMix-style triggers; multistreaming (YouTube and Telegram via API,
Instagram and TikTok by key) with a merged chat; crash-safe recorder; 16:9 or 9:16 projects; colour
correction (lift/gamma/gain/offset, curves, LUT), keying, deinterlacing; hotkeys and OSC for every
function; load monitor and input analytics; MyOBS Encoder — a separate background transcoder.

**Testing.** Every stage is gated by automatic tests: each test frame carries its own number, and every
frame of the program output is checked for flashes, order and pauses.

**Made with [Claude Code](https://claude.com/claude-code)** (Anthropic), model Claude Opus 5.5, medium
reasoning effort. A human sets the goals, tests each stage on real broadcast hardware and makes the
decisions; Claude writes the code, the tests and the CI.

## Interface concept

| | |
|---|---|
| ![Tile](docs/concept/tile.png) | ![Composition and Merge](docs/concept/compose.png) |
| Input tile | Compositions and the Merge transition |
| ![Triggers](docs/concept/triggers.png) | ![Streams](docs/concept/streams.png) |
| Triggers | Multistreaming |

![Input settings](docs/concept/input.png)

## Roadmap

1. **Core** — engine on the discrete GPU, own Program/Preview, CI and automatic tests. *(in progress)*
2. **Switcher and inputs** — frame-accurate CUT / Fade / Merge, Clip, camera, image, calls, tile UI.
3. **Audio** — buses, PFL, tile faders, automatic A/V sync test.
4. **Outputs** — multistreaming, recorder, screens and LED walls, overlays.
5. **Triggers, OSC, analytics, MyOBS Encoder, installer.**

## Building

Built automatically on GitHub (**Actions** tab): the OBS engine is built once and cached, then the
program and the automatic tests. The ready program folder is the `myobs-windows` artifact of each run.

Locally on Windows (Visual Studio 2026, CMake 3.28+):

```
pwsh ci/build-engine.ps1
pwsh ci/build-app.ps1
pwsh ci/run-tests.ps1
```

## License

GPL-2.0-or-later for the MyOBS code; the calls module (obs-vdoninja, GPL-3.0) makes the distributed program GPL-3.0. Engine: [OBS Studio](https://github.com/obsproject/obs-studio) (libobs) — thanks to
its authors. Not affiliated with or endorsed by the OBS Project.
