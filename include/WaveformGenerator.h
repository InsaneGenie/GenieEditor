#pragma once

#include <QVector>
#include <QString>

// Downsampled amplitude data for drawing a waveform on the timeline.
// `peaks` covers the ENTIRE source file's audio (not just a trimmed clip
// range) so a single generation can be reused and correctly re-sliced after
// trims or splits — see Timeline's waveform drawing code for the slicing math.
struct WaveformData {
    QVector<float> peaks; // abs-amplitude peaks in [0, 1], one per bucket
    double durationSec = 0.0; // total duration the peaks array spans
};

class WaveformGenerator {
public:
    // Decodes the file's best audio stream and returns downsampled peak
    // data. Returns an empty `peaks` vector (with durationSec == 0) on
    // failure — no audio stream, decode error, unsupported format, etc.
    // Callers should treat that as "no waveform available", not a hard error.
    //
    // NOTE: this decodes the ENTIRE audio stream synchronously — fine for
    // short clips, but for long-form content (see the 50+ hour use case)
    // this should be moved to a background thread (QtConcurrent::run or a
    // QThread) rather than called directly on the UI thread during import.
    static WaveformData generate(const QString& path, int peakCount = 2000);
};
