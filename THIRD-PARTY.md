# Third-party components

GenieEditor is distributed under the GNU General Public License, version 2 or
later — see `LICENSE`. That obligation comes from libmpv, which is GPL and which
GenieEditor links against for preview playback.

The components below are bundled with, or linked by, a GenieEditor release.

| Component | Licence | How it is used |
| --- | --- | --- |
| **mpv / libmpv** | GPLv2 or later | Linked. Video and audio preview playback. This is what makes GenieEditor as a whole GPL. |
| **Qt 6** | LGPLv3 | Linked dynamically. Window, widgets, networking. |
| **FFmpeg libraries** (libavformat, libavcodec, libavfilter, libavutil, libswscale, libswresample) | LGPLv2.1 or later | Linked. Media probing, waveform generation, thumbnails. |
| **ffmpeg executable** | GPLv2 or later (the bundled build includes x264) | Run as a separate process for export. |

### A note on which ffmpeg build to bundle

The gyan.dev **full** build is about 210 MB and dominates the download. The
**essentials** build is a fraction of that and contains everything GenieEditor
actually invokes: H.264 via libx264, AAC, and the standard filter set the
exporter builds its graphs from. Unless you add a feature that needs something
exotic, bundle essentials.

Both are GPL because of x264, so this changes nothing about licensing.
| **whisper.cpp** | MIT | Linked. Offline speech transcription. |
| **ggml** | MIT | Linked, as part of whisper.cpp. |

## Getting the source

GenieEditor's complete source is at:

<https://github.com/InsaneGenie/GenieEditor>

The GPL requires that source be available to anyone who receives a binary. A
public repository satisfies that, which is why the link belongs on the download
page as well as in this file.

Source for the bundled third-party components is available from their own
projects: mpv at <https://github.com/mpv-player/mpv>, FFmpeg at
<https://ffmpeg.org/download.html>, Qt at <https://download.qt.io>, and
whisper.cpp at <https://github.com/ggerganov/whisper.cpp>.

## Replacing the Qt libraries

Qt is used under the LGPL, which requires that you be able to swap it for your
own build. Qt is linked dynamically and its DLLs sit beside `GenieEditor.exe`,
so replacing them with compatible Qt 6 builds of the same version is enough —
nothing needs to be relinked.

## Not bundled

The whisper speech model (`ggml-base.en.bin`, roughly 140 MB) is **not**
included. It is far larger than the rest of the application and most people
never turn transcription on, so the app asks for it only when the feature is
first used. Models are at
<https://huggingface.co/ggerganov/whisper.cpp/tree/main>; put one in a `models`
folder next to `GenieEditor.exe`, or point the app at it when prompted.