#include "OverlayImageLoader.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {

struct CacheEntry {
    QImage image;
    qint64 modifiedMs = 0;
    qint64 sizeBytes = 0;
};

QHash<QString, CacheEntry> g_cache;
QMutex g_cacheMutex;

} // namespace

QImage OverlayImageLoader::load(const QString& path) {
    // Decoding on every call was a genuine performance bug rather than a
    // theoretical one: this is reached from the 16ms overlay sync AND several
    // times per mouse-move while dragging an overlay's handles, so a single
    // drag was re-reading and re-decoding the PNG from disk dozens of times a
    // second. QImage is implicitly shared, so handing back a cached copy costs
    // a refcount bump rather than a memcpy.
    const QFileInfo info(path);
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = info.size();

    QMutexLocker lock(&g_cacheMutex);

    auto it = g_cache.constFind(path);
    if (it != g_cache.constEnd()
        && it->modifiedMs == modifiedMs
        && it->sizeBytes == sizeBytes) {
        return it->image;
    }

    // Keyed on modification time and size as well as path, so re-exporting a
    // title from another program and switching back picks up the new version
    // instead of showing a stale one until restart.
    CacheEntry entry;
    // Cached already premultiplied: it's what mpv's overlay-add requires, and
    // scaling and rotating in premultiplied space also avoids the dark halo that
    // straight-alpha filtering produces around a PNG's transparent edges.
    // Converting once here rather than per frame keeps it off the render path.
    entry.image = QImage(path).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    entry.modifiedMs = modifiedMs;
    entry.sizeBytes = sizeBytes;
    g_cache.insert(path, entry);
    return entry.image;
}

void OverlayImageLoader::clearCache() {
    QMutexLocker lock(&g_cacheMutex);
    g_cache.clear();
}
