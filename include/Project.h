#pragma once

#include <QString>
#include <QColor>
#include <QVector>
#include <QImage>
#include <memory>
#include <cmath>

// One keyframe: a value pinned to a moment. Times are relative to the CLIP's
// own start, not the project timeline, so dragging a clip carries its animation
// along with it instead of leaving the motion behind at the old position.
struct Keyframe {
    double timeSec = 0.0;
    double value   = 0.0;
};

// A single animatable number. With no keyframes it's just `staticValue`; with
// one it's a constant at that key; with two or more it interpolates linearly
// between them and holds flat outside the first and last.
//
// This is the ONE definition of what a property is worth at a given moment, and
// both the preview and the exporter derive from it — the preview by calling
// valueAt() per frame, the exporter by compiling the same keyframes into an
// FFmpeg expression. That shared origin is what keeps the render honest: if the
// two ever disagree, it's a bug in the compiler, not a difference of opinion
// about the animation.
struct AnimatedProperty {
    QVector<Keyframe> keys; // kept sorted by timeSec
    double staticValue = 0.0;

    bool isAnimated() const { return keys.size() >= 2; }

    double valueAt(double tSec) const {
        if (keys.isEmpty()) return staticValue;
        if (keys.size() == 1 || tSec <= keys.first().timeSec) return keys.first().value;
        if (tSec >= keys.last().timeSec) return keys.last().value;

        for (int i = 1; i < keys.size(); ++i) {
            if (tSec <= keys[i].timeSec) {
                const Keyframe& a = keys[i - 1];
                const Keyframe& b = keys[i];
                const double span = b.timeSec - a.timeSec;
                if (span <= 1e-9) return b.value;
                const double f = (tSec - a.timeSec) / span;
                return a.value + (b.value - a.value) * f;
            }
        }
        return keys.last().value;
    }

    // Inserts a key, replacing any existing one at effectively the same moment
    // so repeatedly keying at the playhead updates rather than stacking.
    void setKey(double tSec, double v) {
        for (int i = 0; i < keys.size(); ++i) {
            if (std::abs(keys[i].timeSec - tSec) < 0.001) {
                keys[i].value = v;
                return;
            }
            if (keys[i].timeSec > tSec) {
                keys.insert(i, Keyframe{tSec, v});
                return;
            }
        }
        keys.push_back(Keyframe{tSec, v});
    }

    bool removeKeyNear(double tSec, double tolSec = 0.05) {
        for (int i = 0; i < keys.size(); ++i) {
            if (std::abs(keys[i].timeSec - tSec) <= tolSec) {
                keys.remove(i);
                return true;
            }
        }
        return false;
    }

    bool hasKeyNear(double tSec, double tolSec = 0.05) const {
        for (const auto& k : keys) {
            if (std::abs(k.timeSec - tSec) <= tolSec) return true;
        }
        return false;
    }
};

// Transform + opacity animation for an Overlay-track clip.
//
// All of these are RESOLUTION-INDEPENDENT on purpose. The preview composites
// against mpv's video surface (whatever size the panel happens to be) while the
// export composites against the chosen output canvas, so anything expressed in
// raw pixels would land in a different place in the rendered file than it did
// on screen. Normalised fractions mean the two agree by construction.
struct OverlayAnimation {
    AnimatedProperty x;       // 0..1 — overlay CENTRE across the canvas width
    AnimatedProperty y;       // 0..1 — overlay CENTRE down the canvas height
    AnimatedProperty scale;   // overlay WIDTH as a fraction of canvas width
    AnimatedProperty opacity; // 0..1
    AnimatedProperty rotation; // degrees clockwise, about the overlay's own centre

    OverlayAnimation() {
        x.staticValue = 0.5;
        y.staticValue = 0.5;
        scale.staticValue = 0.35;
        opacity.staticValue = 1.0;
        rotation.staticValue = 0.0;
    }

    bool hasAnyKeys() const {
        return !x.keys.isEmpty() || !y.keys.isEmpty()
            || !scale.keys.isEmpty() || !opacity.keys.isEmpty()
            || !rotation.keys.isEmpty();
    }

    // The ONE definition of what setting a value means, so the inspector's
    // sliders and dragging the overlay directly on the preview can't develop
    // different ideas about it. A property that isn't animated takes the value
    // outright; an animated one records a keyframe at the playhead instead —
    // which is the entire difference between "change this" and "animate this".
    //
    // `canKeyframe` is false when the playhead sits outside the clip, where a
    // keyframe would land at a time that can never be reached.
    static void applyValue(AnimatedProperty& prop, double value, double localSec, bool canKeyframe) {
        if (prop.keys.isEmpty()) {
            prop.staticValue = value;
        } else if (canKeyframe) {
            prop.setKey(std::max(0.0, localSec), value);
        }
    }
};

// A single clip referencing a source media file with in/out points,
// placed at a given position on a track. Nothing is re-encoded until export.
struct Clip {
    QString sourcePath;
    double sourceInSec  = 0.0;   // in-point within the source file
    double sourceOutSec = 0.0;   // out-point within the source file
    double trackPosSec  = 0.0;   // where this clip sits on the track timeline
    double gainDb       = 0.0;   // audio gain, ignored for video-only clips

    // Downsampled amplitude peaks covering the clip's ENTIRE original
    // source file (not just [sourceInSec, sourceOutSec]) — see
    // WaveformGenerator. Storing the full-file peaks rather than a
    // pre-trimmed slice means a split just copies this vector unchanged;
    // only the trim window (sourceInSec/sourceOutSec) needs adjusting, and
    // the renderer re-slices peaks[] against waveformSourceDurationSec at
    // draw time. Empty for clips with no audio or where generation failed.
    QVector<float> waveformPeaks;

    // Per-bucket RMS matching waveformPeaks index for index (or empty, for
    // clips generated before this existed). The renderer fills the RMS as a
    // solid body inside the translucent peak envelope — see
    // WaveformGenerator.h for why one measurement was never enough.
    QVector<float> waveformRms;

    double waveformSourceDurationSec = 0.0;

    // Frame thumbnails covering the clip's ENTIRE original source file,
    // same "generate once, re-slice on trim" pattern as waveformPeaks —
    // see ThumbnailGenerator. Empty for clips with no video or where
    // generation failed.
    QVector<QImage> thumbnails;
    double thumbnailSourceDurationSec = 0.0;

    // Only meaningful for clips on an Overlay track; ignored everywhere else.
    OverlayAnimation anim;

    double durationSec() const { return sourceOutSec - sourceInSec; }
};

// One transcript segment as produced by whisper.cpp, with word-level timing
// so the transcript panel can seek the playhead on click. Defined before
// Track (below) since each Track now owns its own transcript directly.
struct TranscriptWord {
    QString text;
    double startSec = 0.0;
    double endSec   = 0.0;
};

struct TranscriptSegment {
    QString text;
    double startSec = 0.0;
    double endSec   = 0.0;
    QVector<TranscriptWord> words;

    // Which source file this segment's timestamps are relative to. Carried per
    // segment rather than once per track because a track can hold clips from
    // several different files, and each one gets transcribed separately — the
    // old single Track::transcriptSourcePath could only ever describe one of
    // them, so click-to-seek silently mapped against the wrong file.
    QString sourcePath;
};

// Overlay = a static image composited on top of the video output via mpv's
// native overlay-add/overlay-remove commands — see PlayerWidget::setOverlay.
// Distinct from Video/Audio since it has no intrinsic duration or decoded
// frame source of its own.
enum class TrackType { Video, Audio, Overlay };

struct Track {
    TrackType type = TrackType::Video;
    QString name;
    QVector<Clip> clips;
    bool muted = false;
    double gainDb = 0.0; // track-level gain, mixed with per-clip gain on export

    // PREVIEW-only volume, 0-200 (100 = normal), set via the right-click
    // menu on an audio track's header — deliberately separate from gainDb
    // above, which is a dB-based value used only for the export mix and
    // was never wired into live preview. This one directly drives each
    // audio track's AudioPlayer, combined multiplicatively with the master
    // volume slider (see MainWindow::combinedVolumeForTrack).
    int volumePercent = 100;

    // 0 = use the default height (see TimelineMetrics::effectiveTrackHeight).
    // Set by dragging a track's resize handle in TrackHeaderPanel.
    int heightPx = 0;

    // For VIDEO tracks only: index of the audio track that receives this
    // video track's companion audio clip on import (see
    // MainWindow::importVideoFileAt). -1 = none set. Each "+ Video Track"
    // click creates a fresh paired audio track alongside it, so importing
    // onto a specific video track routes its audio to ITS OWN track rather
    // than always dumping into a single shared audio track regardless of
    // which video track was targeted.
    int pairedAudioTrackIndex = -1;

    // Disables the track entirely — distinct from `muted` (which only
    // silences an audio track's OUTPUT while still decoding it): a
    // disabled track is skipped completely by sync logic, as if it had no
    // clips at all. For a video track this means it's excluded from the
    // priority-layering check (lower tracks show through); for an overlay
    // track it stops being composited; for an audio track it's equivalent
    // to always-muted but cheaper (no decode).
    bool enabled = true;

    // Assigned at creation time based on how many tracks of this SAME type
    // already existed — 0 for the first Video track, 1 for the second, etc.
    // See TimelineMetrics::tintForTrackColorIndex, which uses this to give
    // successive same-type tracks visually distinct colors rather than all
    // looking identical.
    int colorIndex = 0;

    // This AUDIO track's own transcript (empty until transcribed) — owned
    // directly by the track (rather than one flat project-wide list) so
    // each audio track can have an independent transcript shown in its own
    // tab in the Transcript panel. Transcribing a video clip stores its
    // result on that video track's pairedAudioTrackIndex rather than here.
    QVector<TranscriptSegment> transcript;

    // Which source file transcript's timestamps are relative to — whisper
    // transcribes the WHOLE file, not just what's kept in the edit, so a
    // word's startSec is a position within THIS FILE, not a timeline
    // position. Needed to map it back through whichever clip on this track
    // actually references this file (accounting for that clip's own trim
    // window and its position on the timeline) — see
    // MainWindow::mapSourceTimeToTimelineSec.
    QString transcriptSourcePath;

    // The set of source files this track's transcript was built from, joined
    // into one string. Auto-transcription compares this against the track's
    // current sources to decide whether there's anything to redo — which is
    // what stops it re-running on every unrelated edit. Deliberately keyed on
    // WHICH FILES are present, not on clip positions or trims: whisper
    // transcribes whole files and the result is mapped back through each clip's
    // trim window at display time, so moving or trimming a clip cannot change
    // what the transcript should say.
    QString transcriptSignature;
};

// A tagged time range on the timeline — used for markers, highlights,
// and transcript-derived regions.
//
// A marker with zero length (startSec == endSec) is a PIN: a single saved
// position the user dropped at the playhead, rather than a region. Both live in
// the same vector because they're the same idea at different durations, and
// keeping one list means anything that iterates markers (rendering, and later
// export or navigation) handles both without special-casing.
struct Marker {
    double startSec = 0.0;
    double endSec   = 0.0;
    QString label;
    QColor color = QColor(255, 200, 0);

    // Tolerance rather than exact equality: positions come from division by
    // pixels-per-second, so a pin's two ends can differ in the last bits even
    // when it was created as a single point.
    bool isPin() const { return (endSec - startSec) < 0.0005; }
};

class Project {
public:
    QVector<Track> tracks;
    QVector<Marker> markers;

    Track& addTrack(TrackType type, const QString& name) {
        Track t;
        t.type = type;
        t.name = name;
        for (const auto& existing : tracks) {
            if (existing.type == type) ++t.colorIndex;
        }
        tracks.push_back(t);
        return tracks.back();
    }

    // Splits the clip at trackIndex/clipIndex at the given absolute timeline
    // position, producing two clips in place of one. This is the "cut" op.
    bool splitClipAt(int trackIndex, int clipIndex, double timelinePosSec);

    // Removes a track entirely and fixes up any OTHER track's
    // pairedAudioTrackIndex that referenced it (cleared to -1) or came
    // after it (decremented by one to match the shift). Does NOT touch
    // anything outside the Project itself — callers (MainWindow) are
    // responsible for re-indexing their own per-track bookkeeping
    // (AudioPlayer instances, active overlay tracking, current
    // selection/playback state), since Project has no knowledge of those.
    void removeTrack(int trackIndex);

    // Total project duration = end of the furthest clip across all tracks.
    double durationSec() const;
};
