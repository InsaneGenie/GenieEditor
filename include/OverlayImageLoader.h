#pragma once

#include <QImage>
#include <QString>
#include <QVector>

// One decoded animated image: every frame, plus when each frame starts within
// a single loop.
//
// A still is just the degenerate case — one frame, zero loop length — so every
// caller can go through the same path rather than branching on whether the file
// happens to move.
struct OverlayFrames {
    QVector<QImage> frames;   // premultiplied ARGB, all the same size
    QVector<double> startSec; // each frame's start within one loop; same length as frames
    double loopSec = 0.0;     // one full pass; 0 for a still

    bool isAnimated() const { return frames.size() > 1 && loopSec > 0.0; }

    // The frame showing at `localSec` into the clip, LOOPING — a clip stretched
    // past the source's own length keeps playing rather than freezing on the
    // last frame, which matches what the export does and what a GIF is for.
    // Returns -1 when there are no frames at all.
    int indexAt(double localSec) const;
};

// Loads a still image for use as a video overlay.
//
// NOTE: PDF support was considered but dropped — Qt's PDF module isn't a
// standalone vcpkg port; it's bundled inside qtwebengine behind a "pdf"
// feature, since Qt's PDF renderer reuses Chromium's PDF library
// internally. That would mean building a chunk of Chromium just for this
// one feature — disproportionate, so this only handles plain image files
// (PNG/JPG/etc) for now. If PDF support is wanted later, a lighter
// standalone library like Poppler (real vcpkg port, no Chromium) would be
// the way to add it back in.
class OverlayImageLoader {
public:
    // The file's FIRST frame, or a null QImage on failure (missing/corrupt).
    //
    // This is what callers that only need the image's shape want — the
    // inspector's thumbnail, and the stage widget's aspect ratio for placing
    // drag handles. Every frame of an animation shares one canvas size, so
    // frame zero answers those questions for a GIF exactly as well as for a PNG.
    static QImage load(const QString& path);

    // Every frame, for callers that need to show the right one at a given
    // moment — which in practice means the overlay compositor.
    //
    // Decoding is bounded on two axes. Frames are downscaled if the file is
    // large enough that holding all of them would be unreasonable, and the
    // cache as a whole has a byte budget that evicts least-recently-used
    // entries. Neither is a limit anyone should notice with the sort of GIF
    // that gets dropped on a timeline; both matter because a long animation at
    // full resolution can be hundreds of megabytes of uncompressed frames, and
    // this is held for as long as the clip exists.
    static OverlayFrames loadFrames(const QString& path);

    // Drops every cached image. Not needed in normal use (entries invalidate
    // themselves when the file changes) — this is for reclaiming memory.
    static void clearCache();

    // Whether Qt reports more than one frame in this file. Cheap: reads the
    // header only, and doesn't decode or populate the frame cache.
    static bool isAnimated(const QString& path);
};
