#pragma once

#include <QString>
#include <QVector>
#include <QHash>
#include <atomic>
#include <functional>
#include "Project.h"

// How much the picture changes over time, for one source file.
//
// Like WaveformData and ThumbnailStrip this covers the ENTIRE file rather than
// any one clip, so splits, trims and ripple deletes never need a re-analysis:
// the detector just re-reads the profile through each clip's current trim
// window.
//
// Each sample is the FRACTION OF THE FRAME that visibly changed since the
// previous sample (0 = identical, 1 = every pixel different). That measure was
// chosen over a mean pixel difference because it ignores the two things that
// make "is anything happening" hard: sensor noise and compression shimmer
// change every pixel by a tiny amount, which a mean picks up and this doesn't;
// and a mouse cursor or blinking caret changes a few pixels by a lot, which a
// mean dilutes into nothing anyway but which this reports honestly as "0.02%
// of the frame" so the threshold can decide.
struct MotionProfile {
    QVector<float> changedFraction; // one per sample, in [0, 1]
    double intervalSec = 0.25;      // spacing between samples
    double durationSec = 0.0;

    bool isValid() const { return !changedFraction.isEmpty() && durationSec > 0.0; }

    // Highest change reading anywhere in [fromSec, toSec] of the source.
    // Max rather than average: one frame of real motion inside a window means
    // the window is not downtime, however still the rest of it was.
    float maxChangeBetween(double fromSec, double toSec) const;
};

struct DowntimeSettings {
    // A moment counts as still when less than this percentage of the frame
    // changed. 0.5% lets a moving cursor through on a 1080p screen recording
    // while still catching someone shifting in their chair on camera.
    double stillThresholdPct = 0.5;

    // Shorter still stretches are left alone — a one-second pause is pacing,
    // not dead air.
    double minDurationSec = 2.0;

    // Kept at each end of a region, so a cut lands just after the motion stops
    // and just before it starts again rather than exactly on the edge — which
    // is what makes an automated cut feel like a jump instead of a glitch.
    double paddingSec = 0.25;

    // When set, a still picture with someone talking over it is NOT downtime.
    // That's the common case for slide decks and tutorials, where the screen
    // holds still precisely because the narration is doing the work.
    bool requireSilence = true;
    double silenceRms = 0.02; // RMS in [0,1], matching WaveformData::rms
};

class MotionAnalyzer {
public:
    using ProgressFn = std::function<void(double fraction)>; // 0..1 through THIS file

    // Decodes the file's video stream once, start to finish, sampling it
    // `samplesPerSec` times a second at a small greyscale resolution. Returns
    // an invalid profile for audio-only files, decode failures, or when
    // `cancel` is raised part-way.
    //
    // Runs synchronously — call it from a worker thread. Cached by file
    // identity (path + mtime + size), so asking again after an edit is free.
    static MotionProfile analyze(const QString& path,
                                 const std::atomic_bool* cancel = nullptr,
                                 const ProgressFn& progress = {},
                                 double samplesPerSec = 4.0);

    // True if a valid profile for this exact file is already cached — used to
    // skip the progress dialog when nothing would need decoding.
    static bool isCached(const QString& path);

    static void clearCache();

    // Maps each file's profile through the clips that currently use it and
    // returns the timeline ranges where nothing moves.
    //
    // A moment is downtime only when EVERY enabled video clip covering it is
    // still (so a picture-in-picture that's moving keeps the moment), and, if
    // requireSilence is set, every audible audio clip covering it is quiet.
    // Moments with no video at all are gaps, not downtime, and are never
    // reported.
    static QVector<DowntimeRegion> findDowntime(const Project& project,
                                                const QHash<QString, MotionProfile>& profiles,
                                                const DowntimeSettings& settings);
};
