#pragma once

#include <QString>

// Thin wrapper around FFmpeg's libavformat for pulling basic metadata out of
// a media file without a full decode — currently just duration, since that's
// what the timeline needs to size clips correctly on import.
class MediaProbe {
public:
    // Returns the file's duration in seconds, or -1.0 if it couldn't be
    // opened/probed (corrupt file, unsupported container, bad path, etc).
    static double probeDurationSeconds(const QString& path);

    // Resolution and frame rate of a file's first video stream. Used to default
    // the export canvas to whatever the footage actually is, rather than forcing
    // every project through a hardcoded 1080p30.
    struct VideoInfo {
        int width = 0;
        int height = 0;
        double fps = 0.0;
        bool valid() const { return width > 0 && height > 0; }
    };

    static VideoInfo probeVideoInfo(const QString& path);

    // Whether the file actually carries at least one audio stream.
    //
    // Import uses this to decide whether a companion audio clip is worth
    // creating at all — a GIF (or any silent source) has nothing to put on an
    // audio track, and an empty clip there is just something to look at and
    // then delete by hand.
    //
    // Returns TRUE when the file can't be probed. An unreadable file is the one
    // case where guessing wrong is expensive: dropping the audio clip for a
    // source that turns out to have audio silently loses it, while creating a
    // spare empty one is merely untidy. So failure keeps the old behavior.
    static bool hasAudioStream(const QString& path);
};
