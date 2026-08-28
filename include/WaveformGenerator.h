#pragma once

#include <QVector>
#include <QString>

// Downsampled amplitude data for drawing a waveform on the timeline.
// `peaks` covers the ENTIRE source file's audio (not just a trimmed clip
// range) so a single generation can be reused and correctly re-sliced after
// trims or splits — see Timeline's waveform drawing code for the slicing math.
//
// TWO measurements are kept per bucket, not one, because they answer different
// questions and a waveform drawn from either alone is worse than useless for
// editing. `peaks` is the loudest single sample in the bucket — that's what
// finds a transient, a click, the exact frame a word starts. `rms` is the
// bucket's average energy, which is what actually tracks perceived loudness:
// a snare hit and a sustained vowel can share a peak while sounding nothing
// alike, and only the RMS tells them apart. Drawing the RMS as a solid body
// inside a translucent peak envelope is the shape every serious editor uses,
// and it's why their waveforms read as detailed rather than as bars.
struct WaveformData {
    QVector<float> peaks; // loudest abs sample per bucket, in [0, 1]
    QVector<float> rms;   // root-mean-square per bucket, in [0, 1]; same length as peaks
    double durationSec = 0.0; // total duration the arrays span
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
    // `peakCount` of 0 (the default) picks a bucket count from the file's own
    // duration instead of using one fixed number for everything. A flat 2000
    // buckets meant a ten-minute recording got one measurement per 0.3s, so
    // every bucket smeared across many pixels and the result was the wide flat
    // bars this replaces. Resolution now scales with length, with a ceiling so
    // a multi-hour file can't allocate without bound.
    //
    // Peak arrays are held in QVector, which is implicitly shared — splitting a
    // clip copies the Clip struct but NOT the samples, so the higher resolution
    // costs one allocation per imported file rather than one per clip.
    static WaveformData generate(const QString& path, int peakCount = 0);

    // Results are cached by file identity, because generating one decodes the
    // whole audio stream and the same source is routinely asked for more than
    // once -- a video clip and its companion audio share a file, and splitting
    // a clip leaves both halves pointing at it. Entries invalidate themselves
    // when the file changes; this is only for reclaiming memory.
    static void clearCache();
};
