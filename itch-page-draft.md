# itch.io page — draft copy

Edit freely. Everything below is a starting point, not a template to follow
literally.

---

## Title

**GenieEditor**

## Short description (the one-liner under the title, ~140 chars)

> A desktop video editor built for meme edits and commentary videos. Drop in
> GIFs and sound effects without leaving the timeline.

The temptation is to write "a free and open-source non-linear video editor for
Windows". Resist it. That describes Kdenlive, Shotcut, OpenShot and a dozen
others, all free, all more mature. Nobody is looking for another one. What is
actually unusual here is the built-in GIF and sound-effect libraries and the
automatic transcription — lead with those.

## Classification

- **Kind of project:** Tools
- **Release status:** In development
- **Platform:** Windows
- **Pricing:** No payments, with donations enabled — see the note at the bottom

## Tags

`video-editing`, `video-editor`, `tools`, `open-source`, `windows`,
`content-creation`, `meme`

---

## Long description

### What it is

GenieEditor is a video editor for the kind of video that needs a reaction GIF
at 0:43 and a vine boom at 0:44. Multi-track timeline, real preview playback,
H.264 export — with a GIF browser and a sound-effect library built into the
editor instead of a folder of downloads on your desktop.

It is a solo project, written in C++ with Qt and libmpv. It is free, and the
source is public.

### What's in it

**Timeline that works the way you expect**
Multi-track video, audio and overlay lanes. Drag clips between tracks, trim,
split, snap to edges and to the playhead. Detailed waveforms showing peak and
RMS, so you can find a word by looking at it.

**GIFs, without the round trip**
Search a GIF library from inside the editor and drag a result straight onto the
timeline. Scale, position, rotate and fade it over your footage, with
keyframes. It animates in the preview and in the export.

**Sound effects, same idea**
Search, click to audition, drag onto an audio track. Preview volume is separate
from your project audio, so auditioning a library does not blow your ears out.

**Automatic transcription**
Audio is transcribed in the background, on your machine, using whisper.cpp —
nothing is uploaded anywhere. Click a line to jump the playhead to it, or search
the transcript to find the moment you half-remember.

**Speed changes**
Set any clip from 0.05× to 100×. Audio pitch is preserved, so a 4× section still
sounds like speech instead of a chipmunk.

**Projects that reopen where you left them**
Save and load, with playhead position and zoom level restored. Reopens your last
project on launch. Media is tracked by both absolute and relative path, so
moving a project folder does not break every clip in it.

### What it is not

Being straight with you, because a tools page that oversells wastes everybody's
time:

- **This is early software.** One developer, in active development. Expect rough
  edges and save often.
- **Windows only** at the moment.
- **Not a Premiere replacement.** No colour grading, no compositing, no effects
  beyond overlay transforms, no nested sequences, no proxy workflow.
- **No speed ramping yet** — clip speed is constant per clip, not keyframed.
- **Transcription needs a model download** (about 140 MB), and the app will
  prompt you the first time you use it. Everything else works without it.
- Exporting needs `ffmpeg`. It is bundled, so this should just work — but if the
  export dialog says it cannot be found, that is why.

### Requirements

- Windows 10 or 11, 64-bit
- No installation — unzip and run `GenieEditor.exe`

### Source

<https://github.com/InsaneGenie/GenieEditor>

Licensed under the GPL v2 or later. Bug reports and pull requests welcome.

---

## Notes on pricing

Start at **free with donations enabled** rather than a fixed price.

The licence is the reason. GenieEditor links libmpv, which is GPL, so the whole
application is GPL. Selling GPL software is entirely legal — but every buyer may
legally redistribute what they bought, so a price tag mostly buys you support
obligations rather than revenue. Paid GPL tools work when there is an established
audience that wants to fund the work; that is a thing to grow into, not to
launch with.

Free also gets you the thing you actually need right now, which is people using
it and telling you what breaks.

itch's "No payments, donations accepted" setting does exactly this. You can add a
suggested amount, and switch to paid later if it takes off.

---

## Screenshot checklist

itch shows the first image large, so make it count.

1. **The full editor with a real project loaded.** Timeline populated, waveforms
   visible, a video frame in the preview. Never an empty state.
2. **The GIF panel open with results**, mid-drag onto the timeline if you can
   catch it — this is the feature that distinguishes the whole tool.
3. **The sound panel** with search results.
4. **The transcript panel** beside the timeline, showing timestamps.
5. **A clip's speed menu open**, showing the presets.

A 20–30 second silent GIF or an embedded YouTube clip of an actual edit being
made will do more than all five screenshots together. itch supports a YouTube
embed at the top of the page.

Use a real project with real footage, not `testsrc` patterns. People judge a
tool by whether it looks like it has been used.
