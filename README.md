# GenieEditor

A lightweight NLE skeleton: multi-track timeline, libmpv preview playback,
FFmpeg-based export pipeline, and a whisper.cpp transcription hook.

## UI / theming

All visuals come from `include/Theme.h` + `src/Theme.cpp`: the colour tokens, the
Qt stylesheet, the dark palette, and a set of vector icons drawn in code (no
`.qrc`, no image assets). `Theme::apply()` is called once from `main.cpp` before
any widget exists.

Three colour roles, deliberately kept from colliding:

- **neutral slate** — all app chrome
- **cyan** — interaction feedback: focus, selection, snap guides, active toggles
- **amber** — reserved exclusively for *time*: the playhead and its readout,
  in-progress transcription

Track-type hues (video/audio/overlay) sit outside those roles because they encode
content identity rather than interaction state. `TimelineMetrics::trackAccent()`
is the single source for them, shared by `Timeline` and `TrackHeaderPanel` so a
track looks identical on both sides of the scroll seam.

Two things worth knowing before editing the styling:

- There is **no blanket `QWidget { background: ... }` rule**. `PlayerWidget`
  hands a native window handle to mpv, and a global background rule can fight
  mpv's rendering into that surface. Base surfaces come from the QPalette.
- Any `setStyleSheet()` on a container **must be scoped by selector**
  (e.g. `QWidget#PlayerControlBar { ... }`). An unscoped rule propagates to every
  child and silently overrides their app-level styling.

`MainWindow::kLayoutVersion` forces the new default dock arrangement through once
on upgrade; **View → Reset panel layout** restores it on demand.

## Overlay animation

Overlay clips animate on **position, size and opacity**. Select one on an overlay
track and the Overlay inspector (tabbed with Transcript) appears. Each row's
diamond decides whether that property is animated: off, the slider sets a fixed
value; on, moving the slider writes a keyframe **at the playhead**. Keyframes
show as amber diamonds along the bottom of the clip.

Everything is stored resolution-independently — x/y are 0..1 fractions of the
canvas addressing the overlay's *centre*, size is the overlay's width as a
fraction of canvas width. The preview composites against mpv's video surface
while the export composites against the output canvas; anything in raw pixels
would land somewhere different in the rendered file than it did on screen.

`AnimatedProperty` is the single definition of what a property is worth at a
given moment. The preview calls `valueAt()` per frame; the exporter compiles the
same keyframes into an FFmpeg expression via
`FFmpegExporter::compileExpression()`. If the two ever disagree it's a bug in the
compiler, not a difference of opinion about the animation.

### No window toolbar

Import and Export live in the File menu only (with Ctrl+I / Ctrl+E), and
everything else moved into the timeline strip, so the `QToolBar` is gone
entirely rather than left as a near-empty band. `kLayoutVersion` is bumped so
existing installs don't restore a saved layout referencing a toolbar that no
longer exists.

## KLIPY GIF search

The **GIFs** dock (tabbed with Media) searches KLIPY and imports the result you
pick — chat-app interaction, but "pick" downloads and adds to the timeline
instead of sending.

**Why KLIPY and not Tenor:** Google shut the public Tenor API down on
30 June 2026, and stopped issuing new API keys on 13 January 2026 — a Tenor
integration written today could never have worked at all. KLIPY is the
replacement Discord and others moved to.

- **An API key is required** (free, from the KLIPY Partner Panel). None is
  compiled in: a key embedded in a distributed binary is both a terms violation
  and trivially extractable. A *test* key allows 100 requests/hour, which is why
  searches are debounced and in-flight requests are aborted.
- **Endpoint:** `https://api.klipy.com/v2/search`. This is KLIPY's documented
  Tenor-compatibility surface — their published migration is to swap the host
  and keep the call identical. It's used deliberately in preference to the
  native `/api/v1/{key}/gifs/search` route, because the compat request and
  response objects are the ones KLIPY publishes in full; the native response
  shape isn't something to guess at.
  `parseSearchResponse` accepts **either** envelope (`results[].media_formats`
  or `data[].file`, `id`/`slug`, `content_description`/`title`), so moving to
  the native route is a one-line change to `searchEndpoint()`.
- **Attribution:** KLIPY's guidelines require "Search KLIPY" as the search
  placeholder and a visible "Powered by KLIPY". Both are compliance
  requirements, not wording choices — don't change them.
- HTTP 429 is reported specifically as the test-key rate limit, since hitting
  100/hour while experimenting otherwise looks like a broken integration.
- **GIFs import onto a VIDEO track, not an overlay track.** Overlay clips are a
  single still `QImage` plus animated transforms, so an animated GIF placed
  there would show one frozen frame. The video path decodes it properly in both
  the mpv preview and the FFmpeg export. The tradeoff is no transparency.
- Previews **animate on hover only** — thirty simultaneously animating GIFs is
  real CPU load when you can only look at one.
- Downloads are cached under the app data dir and reused; a project references
  the file by path, so it must outlive a temp directory.
- `KLIPY_BASE_URL` redirects the endpoint for testing against a local server.

### Places (pinned folders)

The media browser has a collapsible **PLACES** list: the OS's own standard
folders (Desktop, Downloads, Documents, Pictures, Music, Videos) followed by
whatever the user has pinned. Media lives in a handful of folders you return to
constantly, and clicking "up" repeatedly to reach them is the sort of friction
that makes a built-in browser feel worse than the file manager it replaces.

- The pin button sits with the other navigation controls, since pinning is
  something you do TO the folder you're currently in. It's disabled on standard
  folders, which are already listed — pinning one would just duplicate it.
- Right-click a pinned entry to remove it. Standard folders aren't removable.
- The entry matching the current folder is highlighted, so the list doubles as a
  "where am I" indicator rather than only a set of jump targets.
- Pins persist in QSettings; entries whose folder no longer exists are pruned on
  load so the list can't accumulate dead links.
- Standard folders come from `QStandardPaths`, not hardcoded paths — they're
  relocatable, and on Windows several typically live under OneDrive.
- Jumping to a place calls `setRootPath` as well as navigating: QFileSystemModel
  only populates beneath its root, so hopping outside the current tree without
  it would show an empty folder.

### Media browser previews

`MediaThumbnailProxyModel` generates previews for **images as well as video** —
previously stills fell through to QFileSystemModel's generic file-type icon,
which made a folder of screenshots unreadable.

Stills go through the same async path as video rather than being loaded inline:
a folder of screenshots is exactly where synchronous decoding hurts most, since
each is a multi-megapixel PNG and dozens can be visible at once. `QImageReader`
is asked to scale *during* decode via `setScaledSize` rather than loading full
resolution and shrinking afterwards — on a 4000x3000 source that lets the
decoder skip most of the work instead of allocating tens of megabytes per file.
`setAutoTransform` honours EXIF orientation so phone photos aren't sideways.

### Timeline tool strip

Split, Pin, the three add-track buttons and the zoom cluster all live in the
timeline panel's own strip. Every one of them acts on the timeline and nothing
else, so they sit next to what they affect — which leaves the window toolbar as
a short, unambiguous list of project-level actions (Import, Export).

### Direct manipulation on the preview

Selecting an overlay clip puts a transform box on the preview: drag inside to
move, corner handles to resize, the handle above the top edge to rotate
(Shift snaps to 15°). Handles sit on the *rotated* box, and corner scaling works
off distance from the centre so it stays sane at any angle.

`OverlayStageWidget` is a **frameless translucent top-level window** tracking the
video surface's screen geometry, not a child widget. It has to be: mpv is given a
native window handle and owns that window outright — it paints over anything Qt
parents there and receives the mouse input landing on it. A real window of its
own sits above mpv's child window and gets its own input.

Clicks that miss the box are `ignore()`d rather than accepted, so the rest of the
preview isn't dead under an invisible sheet.

The better long-term fix is switching `PlayerWidget` from `wid` embedding to
mpv's render API drawing into a `QOpenGLWidget`, after which ordinary Qt children
would compose over the video and none of this would be needed. That's a rewrite
of the working playback path, which is why it isn't what's done here.

#### Overlay alpha must be PREMULTIPLIED

mpv's `overlay-add` documents its `bgra` format as premultiplied alpha: every
colour component must already be multiplied by the alpha component. Handing it
straight alpha instead composites as `src + dst*(1-a)` using full-brightness
colour, so *lowering* opacity ADDS light — a half-transparent overlay rendered
brighter than an opaque one.

Measured contribution over black, on a bright title card:

| opacity | straight alpha (wrong) | premultiplied (right) |
|---|---|---|
| 100% | 248.3 | 248.3 |
| 75%  | 248.3 | 186.0 |
| 50%  | 247.7 | 124.3 |
| 25%  | 248.3 |  62.3 |

The whole preview path is premultiplied now — `OverlayImageLoader` caches in
`Format_ARGB32_Premultiplied`, so scaling, the opacity fade and rotation all
happen in that space and `PlayerWidget::setOverlay` is handed a buffer that is
already correct. Doing the filtering premultiplied also removes the dark halo
straight-alpha scaling leaves around a PNG's transparent edges.

The export path is unaffected: FFmpeg's `rgba` is straight alpha and its
`overlay` filter handles it correctly, which is why exports faded properly even
while the preview did not.

#### Snapping and direct drag

Dragging the body snaps the overlay to the frame's **edges and centre** on both
axes, with a dashed guide showing which one you've landed on. **Hold Alt to
bypass** it. The magnet is 9 *screen pixels*, not a fraction of the frame, so it
feels identical at any preview size.

Edge snapping uses the ROTATED box's axis-aligned bounds
(`aabbHalfExtentPx()`), not the unrotated size — what should sit flush against
the frame edge is what you can actually see touching it.

**The box interior carries a 4/255 alpha floor, and that is load-bearing.**
`WA_TranslucentBackground` makes this a layered window, and layered windows are
hit-tested per pixel against the alpha channel — where alpha is 0 the cursor
never enters the window and the event goes to mpv underneath. With a fully
transparent interior, moving over the overlay produced no events at all; only
brushing the opaque outline got one through, which set the hover state and
painted the hover wash, which then gave the interior alpha. Hence the original
symptom: you had to touch the edge of the box before you could grab it.

The floor is laid over exactly the regions `gripAt()` treats as interactive
(body, corner handles, rotation handle and its arm), so clickable and grabbable
are the same shape by construction. Everywhere else stays at alpha 0 so misses
still pass through instead of hitting an invisible sheet over the whole video.

The window also uses `WindowDoesNotAcceptFocus` + `WA_ShowWithoutActivating`:
it's a manipulation surface, not somewhere you type, and stealing activation
would break Delete/M on the timeline right after nudging an overlay.

There is deliberately no centre marker drawn on the box. The entire interior has
always been draggable; the cross that used to sit there implied the opposite —
that the centre was the one spot you could grab. A faint accent wash on hover
says "all of this is grabbable" instead.

#### Performance notes for the overlay path

Dragging handles was initially very choppy. Three causes, all fixed — worth
knowing before adding anything else to this path:

1. **`OverlayImageLoader::load()` had no cache.** It's reached from
   `halfExtentPx()`, which `cornerPx()` calls four times per paint and
   `gripAt()` calls again per hit-test — about ten decodes per mouse move, plus
   one per 16ms sync tick while idle. Measured on a 3840x1080 PNG: **24.9 ms per
   decode, ~249 ms per mouse move**. Cached, the same drag path costs
   **0.04 ms/move**. Entries invalidate on the file's mtime and size, so
   re-exporting a title elsewhere still picks up the new version.
2. **`refresh()` called `setGeometry` + `show` + `raise` every tick.** Each is a
   real window-manager call, and `raise()` on a translucent always-on-top window
   flickers visibly. Now only when the geometry actually changed, and `raise()`
   only on the transition into visibility.
3. **Whole-surface repaints.** The stage window is as large as the video, and
   alpha-composited repaints cost per pixel. It now repaints only the union of
   the box's old and new bounds.

Also: overlay compositing drops to `Qt::FastTransformation` while a handle is
held (quality is part of the render cache key, so releasing re-renders properly),
and the timeline no longer repaints on every mouse move — its keyframe diamonds
can't move while the pointer is down.

### Rotation

Degrees, clockwise about the overlay's centre, animatable like the rest. The
preview rotates via `QTransform`; the export uses FFmpeg's `rotate` with
`c=none` and an output canvas widened to the input's diagonal so corners are
never clipped. Both rotate about the centre and position by the centre, so
neither needs to know the other's padding.

Filter order in the overlay chain is `geq` → `scale` → `rotate`. Unlike `geq`,
`rotate` tolerates the per-frame size changes an animated scale produces —
verified, not assumed.

## Export

**Export** renders the whole timeline — video layering, gaps, trims, overlay
animation and mixed audio — to an MP4, with progress, a time estimate and
cancellation.

This drives the **ffmpeg executable** rather than libav* in-process. The filter
graph is the hard part either way and is identical in both designs; driving the
binary buys progress and cancellation nearly free, and a graph you can paste into
a terminal to reproduce a failure. The cost is a runtime dependency: ffmpeg is
looked for next to the application first, then on PATH, and Export fails with a
clear message *before* asking for a filename if it's missing.
`lastFilterGraph()` is shown under Details on any failure.

Two ordering constraints in the overlay chain are load-bearing, not stylistic:

- **`geq` must come before the animated `scale`.** geq cannot handle input whose
  dimensions change per frame, which is exactly what an animated scale produces.
  The reverse order fails with a swscale assertion.
- **Nothing may sit between `geq` and `scale`.** An intermediate `format` filter
  there segfaulted ffmpeg outright rather than erroring cleanly.

Clips are shifted to their timeline position with `tpad` and gated with
`enable=`, which also guarantees every overlay input has frames from t=0 —
without that, `overlay` stalls waiting on a second input that doesn't start until
later.

## Transcription is automatic

There is no Transcribe button and no Clear Transcript button. Audio tracks
transcribe themselves and keep themselves current.

A **debounced scan** (1.2s of quiet) compares each audio track's current source
files against `Track::transcriptSignature` and queues only what's actually out
of date. Jobs run **one at a time** — whisper is heavy enough that running
several at once just makes them all slower.

The signature is keyed on **which files are present**, not on clip positions or
trims. Whisper transcribes whole files and the result is mapped back through
each clip's trim window at display time, so moving or trimming a clip cannot
change what the transcript should say — and therefore must not re-trigger a
multi-minute run. Only adding or removing a source file does.

`TranscriptSegment::sourcePath` is per segment, so one track can hold
transcripts of several different clips and click-to-seek still maps back to the
right file. Segments are sorted by where they land on the timeline, so the panel
reads in the order you'd hear it rather than grouped by whichever file was
transcribed first.

**Automatic work never opens a dialog.** `resolveWhisperModelPath(allowPrompt)`
is called with `false` from the scan; if no model is configured, a "Choose speech
model…" button appears in the transcript panel instead. A background task the
user didn't ask for is not entitled to interrupt with a modal file picker.

## Pins

`M`, or the **Pin** button in the timeline tool strip, drops a pin at the
playhead. Pressing it again on the same pin removes it, and **double-clicking a
pin's flag in the ruler** deletes it. Single-clicking a flag seeks the playhead
exactly to that pin.

Pins are just `Marker`s with `startSec == endSec` (`Marker::isPin()`), so they
share the existing markers vector — anything that iterates markers gets both
pins and range markers without special-casing. `Timeline` owns the add/remove and
emits `markersChanged()` afterwards, matching how clip edits already work.

The "is there already a pin here" test is done in **pixels, not seconds**: at low
zoom two pins a second apart would overlap and be individually unclickable, while
a fixed seconds tolerance would reject legitimately distinct pins when zoomed in.

Pins are cyan rather than amber on purpose — amber is reserved for the playhead
(see the colour roles below), and a pin sharing it would be indistinguishable
from the needle.

## What's here vs. what's a stub

**Solid / real:**
- `Project.h/.cpp` — the EDL data model (tracks, clips, markers, transcript). `splitClipAt` actually works.
- `Timeline.h/.cpp` — full paint + mouse-interaction implementation for the timeline widget.
- `PlayerWidget.h/.cpp` — real libmpv embedding (wid-based), play/pause/seek/position all functional.
- `MainWindow.h/.cpp` — wires everything together, import/split/select flows work end-to-end.

**Stubbed with a clear build-out plan in comments:**
- `FFmpegExporter.cpp` — `buildTrackFilterGraph()` produces correct filter-graph syntax, but the actual avfilter execution (opening inputs, running the graph, muxing output) isn't wired up yet. This is the single biggest remaining chunk of work.
- `Transcriber.cpp` — whisper.cpp isn't linked yet; see the comment block at the top of the file for the exact `FetchContent` snippet and API calls needed.

## Windows setup (vcpkg)

```powershell
git clone https://github.com/microsoft/vcpkg
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install qt6-base qt6-multimedia mpv ffmpeg --triplet x64-windows

cd path\to\GenieEditor
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>\scripts\buildsystems\vcpkg.cmake
cmake --build . --config Debug
```

If `pkg-config` isn't available on Windows (common), CMake will fall back to
the `find_path`/`find_library` path in `CMakeLists.txt` for mpv — you may
need to pass `-DMPV_INCLUDE_DIR=...` and `-DMPV_LIBRARY=...` explicitly if
vcpkg's mpv port doesn't ship a `.pc` file for your triplet.

## Linux setup

```bash
sudo apt install qt6-base-dev qt6-multimedia-dev libmpv-dev \
    libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev \
    libswscale-dev libswresample-dev pkg-config

mkdir build && cd build
cmake ..
cmake --build .
```

## Recommended build order

1. **Get it compiling and running.** At this stage `Export` will show an
   error dialog (expected — see stub notes above) but Import/playback/
   timeline/split should all work.
2. **Wire up real duration probing on import** — right now imported clips
   get a hardcoded 10s duration. Use `avformat_open_input` +
   `avformat_find_stream_info` to get the real duration and drop the
   placeholder in `MainWindow::onImportClicked`.
3. **Fill in `FFmpegExporter::exportProject`** — start with the
   single-track, single-clip case (no filter graph needed, just trim +
   transcode + mux) before tackling the full multi-track filter graph.
4. **Link whisper.cpp and fill in `Transcriber`** — follow the setup
   comment at the top of `Transcriber.cpp`. Download `ggml-base.en.bin`
   or `ggml-small.en.bin` from the whisper.cpp model repo to start.
5. **Wire transcript-word clicks to playhead seeking** — the plumbing
   (`onTranscriptWordClicked`) exists but the `QListWidget` items are
   currently just plain text; you'll want per-word clickable regions,
   which likely means switching to a custom widget or rich-text item
   delegate once this matters to you.
6. **Markers/highlights UI** — the data model (`Marker`) and rendering
   (`Timeline::paintEvent`) already exist; you just need UI to create them
   (e.g. a keyboard shortcut at the playhead, or a context menu on the
   ruler).

## Notes on the audio mixing math

Per-clip and per-track gain are both stored in dB (`Clip::gainDb`,
`Track::gainDb`) and summed before being handed to FFmpeg's `volume` filter
in `buildTrackFilterGraph`. For preview playback, mpv doesn't know about
your per-track gain model at all yet — if you want live gain preview before
export, you'd control it via `mpv_set_property(mpv, "volume", ...)` per
active track, which requires loading tracks as separate mpv audio streams
rather than a single file. That's a bigger lift; for now preview plays back
the raw imported file only.
