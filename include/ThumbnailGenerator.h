#pragma once

#include <QVector>
#include <QImage>
#include <QString>

// A set of frame thumbnails spanning a video file's ENTIRE duration,
// analogous to WaveformData for audio — Timeline maps a clip's trim window
// onto a slice of these at draw time, so splits/trims don't need regeneration.
struct ThumbnailStrip {
    QVector<QImage> frames;
    double durationSec = 0.0;
};

class ThumbnailGenerator {
public:
    // Decodes `frameCount` evenly-spaced frames from the file's best video
    // stream, scaled to thumbW x thumbH. Returns an empty `frames` vector
    // (durationSec == 0) on failure — no video stream, decode error, etc.
    //
    // Seeking is keyframe-based (AVSEEK_FLAG_BACKWARD), not frame-exact —
    // standard for filmstrip previews, and much faster than exact seeking.
    //
    // NOTE: like WaveformGenerator, this runs synchronously and performs
    // several seeks — fine for short clips, but should move to a background
    // thread for long-form content.
    static ThumbnailStrip generate(const QString& path, int frameCount = 12,
                                    int thumbW = 120, int thumbH = 68);
};
