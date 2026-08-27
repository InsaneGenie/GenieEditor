#include "OverlayImageLoader.h"

#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <algorithm>
#include <cmath>

namespace {

struct CacheEntry {
    OverlayFrames frames;
    qint64 modifiedMs = 0;
    qint64 sizeBytes = 0;   // of the file on disk, for invalidation
    qint64 memoryBytes = 0; // of the decoded frames, for the budget
    quint64 lastUsed = 0;
};

QHash<QString, CacheEntry> g_cache;
QMutex g_cacheMutex;
quint64 g_useCounter = 0;

// Longest edge a decoded frame is allowed to keep. An overlay is drawn at a
// fraction of the canvas, so beyond this there's nothing on screen to show the
// extra detail — it would only cost memory, multiplied by the frame count.
constexpr int kMaxFrameEdge = 720;

// Ceiling on ONE file's decoded frames. Anything above it is downscaled
// further, uniformly, rather than dropping frames: losing resolution is barely
// visible at overlay size, while losing frames visibly stutters the animation.
constexpr qint64 kPerFileBudgetBytes = 64ll * 1024 * 1024;

// Ceiling on the cache as a whole, past which least-recently-used entries are
// dropped. They re-decode on next use, so this costs time and never correctness.
constexpr qint64 kCacheBudgetBytes = 256ll * 1024 * 1024;

// GIF stores per-frame delays in hundredths of a second, and a great many files
// in the wild specify 0 or 1 — meaning "as fast as possible", which every
// browser has agreed for decades to interpret as 100ms instead. Matching that
// is what makes a GIF play here at the speed it plays everywhere else.
constexpr int kMinSensibleDelayMs = 20;
constexpr int kDefaultDelayMs = 100;

// Drops least-recently-used entries until the cache fits. Caller holds the lock.
void enforceCacheBudget() {
    qint64 total = 0;
    for (const CacheEntry& e : g_cache) total += e.memoryBytes;
    if (total <= kCacheBudgetBytes) return;

    QVector<QPair<quint64, QString>> byAge;
    byAge.reserve(g_cache.size());
    for (auto it = g_cache.constBegin(); it != g_cache.constEnd(); ++it) {
        byAge.push_back({it->lastUsed, it.key()});
    }
    std::sort(byAge.begin(), byAge.end());

    for (const auto& entry : byAge) {
        if (total <= kCacheBudgetBytes) break;
        auto it = g_cache.constFind(entry.second);
        if (it == g_cache.constEnd()) continue;
        total -= it->memoryBytes;
        g_cache.remove(entry.second);
    }
}

OverlayFrames decode(const QString& path) {
    OverlayFrames out;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return out;

    // Work out the downscale ONCE, before decoding, from the header's reported
    // size and frame count. Deciding per frame would risk different frames
    // landing at different sizes, which the compositor's aspect maths assumes
    // cannot happen.
    const QSize native = reader.size();
    const int declaredCount = std::max(1, reader.imageCount());
    QSize target;

    if (native.isValid() && native.width() > 0 && native.height() > 0) {
        double factor = 1.0;

        const int longEdge = std::max(native.width(), native.height());
        if (longEdge > kMaxFrameEdge) {
            factor = static_cast<double>(kMaxFrameEdge) / longEdge;
        }

        const qint64 bytesAtFactor = static_cast<qint64>(native.width() * factor)
                                   * static_cast<qint64>(native.height() * factor)
                                   * 4 * declaredCount;
        if (bytesAtFactor > kPerFileBudgetBytes) {
            // Area scales with the square of the linear factor, hence the root.
            factor *= std::sqrt(static_cast<double>(kPerFileBudgetBytes) / bytesAtFactor);
        }

        if (factor < 1.0) {
            target = QSize(std::max(1, static_cast<int>(native.width() * factor)),
                           std::max(1, static_cast<int>(native.height() * factor)));
        }
    }

    double elapsedSec = 0.0;
    while (reader.canRead()) {
        QImage frame = reader.read();
        if (frame.isNull()) break;

        if (target.isValid() && target != frame.size()) {
            frame = frame.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        // Premultiplied is what mpv's overlay-add requires, and scaling and
        // rotating in premultiplied space also avoids the dark halo that
        // straight-alpha filtering produces around transparent edges.
        // Converting once here rather than per composited frame keeps it off
        // the render path.
        out.frames.push_back(frame.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        out.startSec.push_back(elapsedSec);

        // nextImageDelay() describes the frame just read, so it has to be
        // queried after read() rather than before.
        int delayMs = reader.nextImageDelay();
        if (delayMs < kMinSensibleDelayMs) delayMs = kDefaultDelayMs;
        elapsedSec += delayMs / 1000.0;

        // No jumpToNextImage() here. read() has ALREADY advanced to the next
        // frame — GIF is a sequential format, and its handler reports
        // jumpToNextImage as unsupported, so calling it returns false and would
        // end this loop after a single frame. That failure is quiet and easy to
        // miss: the overlay still appears, it just never moves.
    }

    // A still has no loop length — leaving it at zero is what makes indexAt
    // short-circuit instead of dividing by it.
    out.loopSec = out.frames.size() > 1 ? elapsedSec : 0.0;
    return out;
}

const CacheEntry* fetch(const QString& path) {
    const QFileInfo info(path);
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = info.size();

    auto it = g_cache.find(path);
    if (it != g_cache.end() && it->modifiedMs == modifiedMs && it->sizeBytes == sizeBytes) {
        it->lastUsed = ++g_useCounter;
        return &it.value();
    }

    // Keyed on modification time and size as well as path, so re-exporting a
    // title from another program and switching back picks up the new version
    // instead of showing a stale one until restart.
    CacheEntry entry;
    entry.frames = decode(path);
    entry.modifiedMs = modifiedMs;
    entry.sizeBytes = sizeBytes;
    entry.lastUsed = ++g_useCounter;
    for (const QImage& frame : entry.frames.frames) {
        entry.memoryBytes += static_cast<qint64>(frame.sizeInBytes());
    }

    g_cache.insert(path, entry);
    enforceCacheBudget();

    auto inserted = g_cache.constFind(path);
    return inserted == g_cache.constEnd() ? nullptr : &inserted.value();
}

} // namespace

int OverlayFrames::indexAt(double localSec) const {
    if (frames.isEmpty()) return -1;
    if (frames.size() == 1 || loopSec <= 0.0) return 0;

    double t = std::fmod(std::max(0.0, localSec), loopSec);
    if (t < 0.0) t = 0.0;

    // Binary search rather than a scan: this runs on the 16ms sync tick for
    // every visible overlay, and a long animation is easily hundreds of frames.
    int lo = 0;
    int hi = startSec.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (startSec[mid] <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

QImage OverlayImageLoader::load(const QString& path) {
    QMutexLocker lock(&g_cacheMutex);
    const CacheEntry* entry = fetch(path);
    if (!entry || entry->frames.frames.isEmpty()) return QImage();
    // QImage is implicitly shared, so handing back a cached copy costs a
    // refcount bump rather than a memcpy.
    return entry->frames.frames.first();
}

OverlayFrames OverlayImageLoader::loadFrames(const QString& path) {
    QMutexLocker lock(&g_cacheMutex);
    const CacheEntry* entry = fetch(path);
    return entry ? entry->frames : OverlayFrames();
}

bool OverlayImageLoader::isAnimated(const QString& path) {
    // Header only — deliberately does NOT go through the cache, so asking the
    // question during import doesn't decode every frame of a file that may
    // never end up on an overlay track.
    QImageReader reader(path);
    return reader.canRead() && reader.imageCount() > 1;
}

void OverlayImageLoader::clearCache() {
    QMutexLocker lock(&g_cacheMutex);
    g_cache.clear();
}
