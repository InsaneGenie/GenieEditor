#pragma once

#include <QWidget>

class QFileSystemModel;
class QListView;
class QModelIndex;
class QLabel;
class QPushButton;
class QToolButton;
class QListWidget;
class QListWidgetItem;
class MediaThumbnailProxyModel;

// A simple file-system browser for dragging media files directly onto
// Timeline. Built on QFileSystemModel, which already produces a standard
// text/uri-list drag payload for its rows — Timeline::dropEvent reads the
// dropped file's local path straight from that, no custom mime type needed.
class MediaBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit MediaBrowserPanel(QWidget* parent = nullptr);

signals:
    // Lets double-clicking a file act as an alternative to dragging it —
    // appends to the end of the video track, mirroring the toolbar Import
    // button's behavior.
    void fileActivated(const QString& filePath);

private slots:
    void onChooseFolderClicked();
    void onPinCurrentClicked();
    void onPlaceActivated(QListWidgetItem* item);
    void onUpClicked();
    void onItemDoubleClicked(const QModelIndex& index);

private:
    // Navigates the view into a folder (double-click) or to a fresh
    // directory (Choose Folder), updating the path label either way.
    void navigateTo(const QString& dirPath);

    // --- Places -----------------------------------------------------------
    // The shortcut list at the top of the panel: the OS's own standard folders
    // (Desktop, Downloads, Pictures, …) followed by whatever the user has
    // pinned. Media lives in a handful of places you return to constantly, and
    // clicking "up" repeatedly to reach them is the sort of friction that makes
    // a browser feel worse than the file manager it's replacing.
    void rebuildPlacesList();
    void updatePinButton();
    void loadPinnedPaths();
    void savePinnedPaths() const;

    // Standard OS locations, filtered to those that actually exist.
    static QVector<QPair<QString, QString>> standardPlaces(); // (display name, path)

    QFileSystemModel* m_model = nullptr;
    MediaThumbnailProxyModel* m_proxyModel = nullptr;
    QListView* m_view = nullptr;
    QLabel* m_pathLabel = nullptr;
    QPushButton* m_upButton = nullptr;

    QListWidget* m_placesList = nullptr;
    QToolButton* m_placesHeader = nullptr;
    QToolButton* m_pinButton = nullptr;
    QStringList m_pinnedPaths;
    QString m_currentDir;
};
