#pragma once

#include <QImage>
#include <QString>

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
    // Returns a null QImage on failure (missing/corrupt file).
    //
    // Results are cached, keyed on the file's path, modification time and size —
    // callers hit this on the overlay sync tick and repeatedly during handle
    // drags, so an uncached decode here shows up directly as choppy dragging.
    // Thread-safe; the cache is guarded by a mutex.
    static QImage load(const QString& path);

    // Drops every cached image. Not needed in normal use (entries invalidate
    // themselves when the file changes) — this is for reclaiming memory.
    static void clearCache();
};
