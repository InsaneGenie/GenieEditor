#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QString>

class QApplication;

// =============================================================================
// Theme — the single source of truth for the app's visual language.
// =============================================================================
//
// Everything visual in this app now comes from here: the Qt stylesheet that
// styles the standard widgets (buttons, tabs, scrollbars, docks) AND the raw
// QColor values the custom-painted widgets (Timeline, TrackHeaderPanel) use in
// their paintEvent. That shared origin is the whole point — previously the
// custom widgets hardcoded their own dark greys while the standard widgets
// used the platform's default (light, on Windows) style, so the app looked
// like two different programs stitched together.
//
// --- The colour system ------------------------------------------------------
//
// Three semantic roles, deliberately kept from colliding with each other:
//
//   * NEUTRAL SLATE (bg0..bg4, line, text*) — all app chrome. Slightly cool
//     and never pure grey, so it reads as a considered surface rather than
//     "default dark mode". Chrome recedes; the media is the bright thing.
//   * ACCENT / cyan — "the system is telling you something": focus rings,
//     selection, snap guides, active toggles. One hue for all interactive
//     feedback means you learn it once.
//   * NOW / amber — time and only time: the playhead, its timecode capsule,
//     in-progress transcription. Nothing else is allowed to be amber, which is
//     what makes the playhead findable instantly on a busy timeline.
//
// Track-type colours (video/audio/overlay) sit outside those three roles
// because they encode content identity, not interaction state. They're
// deliberately desaturated relative to the old palette so that thumbnails and
// waveforms — the actual content — stay the most saturated thing on screen.
//
namespace Theme {

// --- Surfaces (darkest to lightest) -----------------------------------------
inline QColor bg0()       { return QColor(0x0B, 0x0D, 0x10); } // deepest — timeline canvas, gaps
inline QColor bg1()       { return QColor(0x10, 0x13, 0x18); } // panel background
inline QColor bg2()       { return QColor(0x16, 0x1A, 0x21); } // raised — toolbar, ruler, track headers
inline QColor bg3()       { return QColor(0x1D, 0x22, 0x2B); } // hover, input fields
inline QColor bg4()       { return QColor(0x27, 0x2E, 0x39); } // pressed, active surface

inline QColor line()      { return QColor(0x2B, 0x32, 0x3E); } // visible borders
inline QColor lineSoft()  { return QColor(0x1C, 0x21, 0x29); } // hairline dividers between lanes

// --- Text --------------------------------------------------------------------
inline QColor text()      { return QColor(0xDF, 0xE4, 0xEC); }
inline QColor textDim()   { return QColor(0x97, 0xA1, 0xB0); }
inline QColor textFaint() { return QColor(0x62, 0x6C, 0x7C); }

// --- Roles --------------------------------------------------------------------
inline QColor accent()    { return QColor(0x4F, 0xD1, 0xE0); } // interaction / selection / snap
inline QColor accentDim() { return QColor(0x2C, 0x7B, 0x87); }
inline QColor now()       { return QColor(0xF5, 0xA5, 0x24); } // playhead and nothing else
inline QColor danger()    { return QColor(0xE5, 0x48, 0x4D); }
inline QColor warning()   { return QColor(0xD9, 0x93, 0x1E); }

// --- Track identity -----------------------------------------------------------
// Returned as a base hue per track type; TimelineMetrics::tintForTrackColorIndex
// then rotates it per track so "Video 2" is distinguishable from "Video 1".
inline QColor videoBase()   { return QColor(0x4A, 0x7A, 0xB8); }
inline QColor audioBase()   { return QColor(0x45, 0x9E, 0x76); }
inline QColor overlayBase() { return QColor(0x8B, 0x5F, 0xB5); }

// --- Metrics ------------------------------------------------------------------
// Corner radii and spacing, kept here rather than scattered as magic numbers so
// the whole app rounds consistently.
constexpr int kRadiusSm = 4;
constexpr int kRadiusMd = 6;
constexpr int kRadiusLg = 9;

// --- Typography ---------------------------------------------------------------
// The UI font is the platform's own (Segoe UI on Windows) — fighting the OS on
// body text buys nothing. The MONO font is the deliberate choice: every
// timecode, tick label and duration in the app is set in it, so digits are
// fixed-width and a running playhead readout doesn't jitter as the numbers
// change. That's the detail that makes a timeline feel like an instrument
// rather than a form.
QFont uiFont(int pointSizeDelta = 0, int weight = QFont::Normal);
QFont monoFont(int pointSizeDelta = 0, int weight = QFont::Normal);

// --- Icons ---------------------------------------------------------------------
// Drawn as vector paths at paint time rather than loaded from image files:
// no asset pipeline, no resource .qrc to keep in sync, crisp at any DPI, and
// recolourable on demand (which is what makes the disabled/hover states work).
// This replaces the emoji that were previously used as button labels — those
// render inconsistently across platforms and fonts, and always look pasted-on.
enum class Icon {
    Import, Export, Split, Transcribe,
    ZoomIn, ZoomOut, ZoomFit,
    AddVideo, AddAudio, AddOverlay,
    Play, Pause, SkipBack, SkipForward, GoToStart, GoToEnd,
    Volume, VolumeMute,
    Search, FolderUp, Folder, Trash, Eye, EyeOff, Clear, Pin,
    Keyframe, KeyframeFilled, PinFilled, ChevronDown, ChevronRight,
};

// `color` defaults to Theme::text() when left invalid.
QIcon icon(Icon which, const QColor& color = QColor(), int sizePx = 20);

// --- Application-wide setup ------------------------------------------------------
// Installs the Fusion style (the only Qt style that honours a custom dark
// palette consistently across platforms — the native Windows style ignores
// most of it), the dark palette, the default font, and the stylesheet.
void apply(QApplication& app);

// The full Qt stylesheet, with @token placeholders already resolved against the
// colours above. Exposed separately mainly so it can be re-applied to a
// dynamically created top-level window if one ever needs it.
QString stylesheet();

} // namespace Theme
