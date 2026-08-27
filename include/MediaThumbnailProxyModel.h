#pragma once

#include <QIdentityProxyModel>
#include <QHash>
#include <QSet>
#include <QIcon>
#include <QPersistentModelIndex>

class QFileSystemModel;

// Wraps QFileSystemModel to supply real previews instead of the generic
// file-type icon QFileSystemModel provides by default: a decoded frame for
// video files (via ThumbnailGenerator), and the image itself for stills. Falls
// back to the base model's icon for everything else (folders, audio files).
//
// Thumbnails are generated lazily and ASYNCHRONOUSLY — the first time Qt
// asks for DecorationRole on a given file, this returns the generic icon
// immediately and kicks off a background QtConcurrent job; once that
// finishes, the real thumbnail is cached and dataChanged() tells the view
// to repaint just that item. Generating synchronously right inside data()
// (the original approach) blocked the UI thread on every newly-visible
// item — exactly what made scrolling/navigating feel laggy until every
// visible thumbnail had already been decoded once.
class MediaThumbnailProxyModel : public QIdentityProxyModel {
    Q_OBJECT
public:
    explicit MediaThumbnailProxyModel(QFileSystemModel* sourceModel, QObject* parent = nullptr);

    QVariant data(const QModelIndex& index, int role) const override;

private:
    static bool isVideoExtension(const QString& suffix);
    static bool isImageExtension(const QString& suffix);
    // const because it's called from data() — see the const_cast inside for
    // why that's fine here (lazily populating a cache from a const getter
    // is a standard, safe pattern).
    void startThumbnailGeneration(const QString& path, const QPersistentModelIndex& sourceIndex) const;

    QFileSystemModel* m_fsModel;
    mutable QHash<QString, QIcon> m_thumbnailCache;
    mutable QSet<QString> m_pendingPaths; // paths currently generating in the background, to avoid duplicate jobs
};

