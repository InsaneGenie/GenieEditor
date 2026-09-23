#pragma once

#include <QString>
#include "WaveformGenerator.h"
#include "ThumbnailGenerator.h"

// On-disk cache for the two expensive per-file visuals: waveform peaks and
// thumbnail strips.
//
// Both are pure functions of a file's contents, and both are costly to make —
// a waveform is a full audio decode, which for an 80-hour recording is many
// minutes of CPU. The in-memory caches already stop that repeating within a
// session, but every project open, and every relaunch, started from scratch.
// With this, a given file is decoded for visuals once, ever, until it changes.
//
// Entries are keyed on path, modification time and size (plus the resolution
// asked for), the same identity the in-memory caches use, so re-exporting a
// file invalidates it automatically. Stored under the per-user cache location
// (%LOCALAPPDATA%\GenieEditor\GenieEditor\cache\visuals on Windows), which is
// safe to delete at any time — the only cost is regenerating.
//
// All functions are thread-safe to call from worker threads: each entry is
// its own file, written atomically, and readers treat anything malformed as a
// miss.
namespace VisualDiskCache {

bool loadWaveform(const QString& mediaPath, int peakCount, WaveformData* out);
void saveWaveform(const QString& mediaPath, int peakCount, const WaveformData& data);

bool loadThumbnails(const QString& mediaPath, int frameCount, int thumbW, int thumbH, ThumbnailStrip* out);
void saveThumbnails(const QString& mediaPath, int frameCount, int thumbW, int thumbH, const ThumbnailStrip& strip);

} // namespace VisualDiskCache
