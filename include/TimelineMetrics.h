#pragma once

#include "Project.h"
#include "Theme.h"
#include <QColor>

// Shared layout constants and geometry helpers so Timeline (the scrollable
// ruler + clip area) and TrackHeaderPanel (the fixed track-name sidebar)
// stay in lockstep — mirrors how Premiere Pro keeps its track-header column
// perfectly aligned with the track lanes, including when tracks are
// individually resized.
namespace TimelineMetrics {
constexpr int kRulerHeight = 32;         // taller than before: the tick labels are now
                                         // set in a mono face with real breathing room,
                                         // plus a dedicated marker band along the bottom
constexpr int kMarkerBandHeight = 5;     // slim colour band at the base of the ruler where
                                         // markers live, so they never fight the playhead
constexpr int kTrackHeight = 74;         // default/fallback track height
constexpr int kTrackHeaderWidth = 190;
constexpr int kClipHeaderBarHeight = 18; // the colored name-strip at the top of each clip
constexpr int kMinTrackHeight = 34;
constexpr int kMaxTrackHeight = 260;

// Cosmetic clip geometry, shared so the drag-preview ghost lands exactly where
// the real clip will once dropped.
constexpr int kClipVMargin = 3;          // gap between a clip and its lane's edges
constexpr int kClipRadius = 5;

// A track's actual height: its own override if set, else the shared default.
inline int effectiveTrackHeight(const Track& track) {
    return track.heightPx > 0 ? track.heightPx : kTrackHeight;
}

// The y-coordinate where a given track's lane begins, accounting for every
// preceding track's (possibly custom) height.
inline int trackTopY(const Project& project, int trackIndex) {
    int y = kRulerHeight;
    for (int i = 0; i < trackIndex && i < project.tracks.size(); ++i) {
        y += effectiveTrackHeight(project.tracks[i]);
    }
    return y;
}

// Sum of every track's effective height — the vertical space the track
// lanes occupy, not counting the ruler strip.
inline int totalTracksHeight(const Project& project) {
    int h = 0;
    for (const auto& t : project.tracks) h += effectiveTrackHeight(t);
    return h;
}

// Rotates a base color's hue based on a track's colorIndex, so successive
// tracks of the same type (e.g. "Video 2" after "Video 1") get visually
// distinct colors rather than all looking identical — used by both
// Timeline (clip fills) and TrackHeaderPanel (accent stripe) with the same
// formula, so a track's color stays consistent between the two widgets.
inline QColor tintForTrackColorIndex(const QColor& base, int colorIndex) {
    if (colorIndex <= 0) return base;
    const int baseHue = base.hue();
    if (baseHue < 0) return base; // achromatic (gray) — nothing to rotate
    constexpr int kHueStepDegrees = 35; // stays within the same general "family" while remaining distinguishable
    const int newHue = (baseHue + colorIndex * kHueStepDegrees) % 360;
    return QColor::fromHsv(newHue, base.saturation(), base.value(), base.alpha());
}

// The one identity colour for a given track, resolved from the theme and then
// hue-rotated for its index. Both Timeline (clip fills, lane tint) and
// TrackHeaderPanel (accent stripe, type label) call this, which is what
// guarantees a track looks like the same track on both sides of the seam —
// previously each widget kept its own near-but-not-quite-matching constants.
inline QColor trackAccent(TrackType type, int colorIndex) {
    QColor base;
    switch (type) {
    case TrackType::Video:   base = Theme::videoBase();   break;
    case TrackType::Overlay: base = Theme::overlayBase(); break;
    default:                 base = Theme::audioBase();   break;
    }
    return tintForTrackColorIndex(base, colorIndex);
}

// A track's lane background: the theme's canvas colour with the faintest wash
// of the track's identity hue mixed in. Just enough that video/audio/overlay
// lanes are separable at a glance, far too little to compete with clip content.
inline QColor trackLaneColor(TrackType type, int colorIndex) {
    const QColor accent = trackAccent(type, colorIndex);
    const QColor canvas = Theme::bg0();
    constexpr double kWash = 0.10;
    return QColor(
        static_cast<int>(canvas.red()   * (1 - kWash) + accent.red()   * kWash),
        static_cast<int>(canvas.green() * (1 - kWash) + accent.green() * kWash),
        static_cast<int>(canvas.blue()  * (1 - kWash) + accent.blue()  * kWash));
}
} // namespace TimelineMetrics
