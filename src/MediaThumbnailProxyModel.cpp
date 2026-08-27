#include "MediaThumbnailProxyModel.h"
#include "ThumbnailGenerator.h"

#include <QFileSystemModel>
#include <QImageReader>
#include <QFileInfo>
#include <QPixmap>
#include <QFutureWatcher>
#include <QtConcurrent>

namespace {
// Matches the media browser's icon size, so a thumbnail is generated at the
// size it's actually displayed at rather than being scaled again on paint.
constexpr int kThumbWidth = 104;
constexpr int kThumbHeight = 58;
} // namespace

MediaThumbnailProxyModel::MediaThumbnailProxyModel(QFileSystemModel* sourceModel, QObject* parent)
    : QIdentityProxyModel(parent), m_fsModel(sourceModel) {
    setSourceModel(sourceModel);
}

bool MediaThumbnailProxyModel::isVideoExtension(const QString& suffix) {
    static const QStringList kVideoExtensions = {"mp4", "mov", "mkv", "avi"};
    return kVideoExtensions.contains(suffix.toLower());
}

bool MediaThumbnailProxyModel::isImageExtension(const QString& suffix) {
    static const QStringList kImageExtensions = {
        "png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    return kImageExtensions.contains(suffix.toLower());
}

void MediaThumbnailProxyModel::startThumbnailGeneration(const QString& path,
                                                         const QPersistentModelIndex& sourceIndex) const {
    if (m_pendingPaths.contains(path)) return; // already generating — don't queue a duplicate job
    m_pendingPaths.insert(path);

    // data() is const, but kicking off async work and later mutating the
    // cache is the standard "lazy population from a const getter" pattern —
    // const_cast is safe here since nothing about the model's logical state
    // (as observed by callers) changes until the result actually arrives.
    auto* self = const_cast<MediaThumbnailProxyModel*>(this);

    auto* watcher = new QFutureWatcher<ThumbnailStrip>(self);
    connect(watcher, &QFutureWatcher<ThumbnailStrip>::finished, self,
            [self, watcher, path, sourceIndex] {
        const ThumbnailStrip strip = watcher->result();
        watcher->deleteLater();
        self->m_pendingPaths.remove(path);

        if (!strip.frames.isEmpty()) {
            self->m_thumbnailCache.insert(path, QIcon(QPixmap::fromImage(strip.frames.first())));
        }

        // Only repaint if this row still exists — the persistent index
        // naturally becomes invalid if the file was removed/renamed while
        // the thumbnail was generating.
        if (sourceIndex.isValid()) {
            const QModelIndex proxyIndex = self->mapFromSource(sourceIndex);
            emit self->dataChanged(proxyIndex, proxyIndex, {Qt::DecorationRole});
        }
    });
    if (isImageExtension(QFileInfo(path).suffix())) {
        // Stills go through the same async path as video rather than being
        // loaded inline. A folder of screenshots is exactly the case where
        // synchronous decoding hurts most — each one is a multi-megapixel PNG,
        // and there can be dozens visible at once.
        watcher->setFuture(QtConcurrent::run([path] {
            ThumbnailStrip strip;

            QImageReader reader(path);
            reader.setAutoTransform(true); // honour EXIF orientation on phone photos

            // Asking the reader to scale during decode, rather than loading full
            // resolution and shrinking afterwards, is the whole difference on a
            // 4000x3000 source: for JPEG especially it lets the decoder skip most
            // of the work instead of allocating tens of megabytes per file.
            const QSize full = reader.size();
            if (full.isValid() && !full.isEmpty()) {
                QSize target = full.scaled(kThumbWidth, kThumbHeight, Qt::KeepAspectRatio);
                if (target.isEmpty()) target = QSize(kThumbWidth, kThumbHeight);
                reader.setScaledSize(target);
            }

            QImage image = reader.read();
            if (image.isNull()) return strip; // unreadable or unsupported — keep the generic icon

            if (image.width() > kThumbWidth || image.height() > kThumbHeight) {
                image = image.scaled(kThumbWidth, kThumbHeight,
                                     Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            strip.frames.push_back(image);
            return strip;
        }));
        return;
    }

    watcher->setFuture(QtConcurrent::run(&ThumbnailGenerator::generate, path,
                                         /*frameCount=*/1, kThumbWidth, kThumbHeight));
}

QVariant MediaThumbnailProxyModel::data(const QModelIndex& index, int role) const {
    if (role == Qt::DecorationRole && index.column() == 0) {
        const QModelIndex sourceIndex = mapToSource(index);
        if (!m_fsModel->isDir(sourceIndex)) {
            const QString path = m_fsModel->filePath(sourceIndex);
            const QString suffix = QFileInfo(path).suffix();
            if (isVideoExtension(suffix) || isImageExtension(suffix)) {
                const auto cached = m_thumbnailCache.constFind(path);
                if (cached != m_thumbnailCache.constEnd()) {
                    return *cached;
                }

                // Not cached yet — kick off background generation (if not
                // already in flight) and return the generic icon for now.
                // dataChanged() fires once the real thumbnail is ready,
                // triggering a repaint of just this item.
                startThumbnailGeneration(path, QPersistentModelIndex(sourceIndex));
                return QIdentityProxyModel::data(index, role);
            }
        }
    }
    return QIdentityProxyModel::data(index, role);
}
