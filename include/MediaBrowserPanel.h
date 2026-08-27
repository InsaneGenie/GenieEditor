#pragma once

#include <QWidget>

class QFileSystemModel;
class QListView;
class QModelIndex;
class QLineEdit;
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

protected:
    // Watches the Places list for the Delete key, so removing an entry doesn't
    // require finding it in a context menu.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onChooseFolderClicked();
    void onPinCurrentClicked();
    void onPlaceActivated(QListWidgetItem* item);
    void onUpClicked();
    void onItemDoubleClicked(const QModelIndex& index);
    // Removes one entry from Places; see the note on the private helper.
    void removePlace(const QString& path);
    // A path was typed into the bar and committed with Enter.
    void onPathEntered();

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
    void loadPlacesConfig();
    void savePlacesConfig() const;

    // Reads the list's CURRENT visual order back into m_placesOrder and saves
    // it. Called after a drag finishes, which is the only thing that reorders.
    void capturePlacesOrder();

    // Briefly marks the path bar as rejecting what was typed, then restores it
    // to the folder actually being shown.
    void flashPathError();

    // Resizes the Places row to exactly the number of wrapped rows its chips
    // need, up to kMaxPlacesRows. Qt won't do this for us: QListView reports a
    // size hint based on its own scroll area, not on how its items happen to
    // wrap at the current width, so the packing has to be worked out here.
    void updatePlacesHeight();

    // Rows of chips to show before the row starts scrolling instead of growing.
    static constexpr int kMaxPlacesRows = 3;

    // Standard OS locations, filtered to those that actually exist.
    static QVector<QPair<QString, QString>> standardPlaces(); // (display name, path)

    QFileSystemModel* m_model = nullptr;
    MediaThumbnailProxyModel* m_proxyModel = nullptr;
    QListView* m_view = nullptr;
    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_upButton = nullptr;

    QListWidget* m_placesList = nullptr;
    QToolButton* m_placesHeader = nullptr;
    QToolButton* m_pinButton = nullptr;

    // Folders the user added themselves.
    QStringList m_pinnedPaths;
    // Standard OS folders the user removed. Kept as an explicit deny-list
    // because standardPlaces() rebuilds from the OS every time.
    QStringList m_hiddenPlaces;
    // Display order, by path, spanning BOTH kinds of entry — dragging one
    // among the other would otherwise have nowhere to be recorded. Paths not
    // listed here are appended, so a newly pinned folder lands at the end
    // rather than in an arbitrary position.
    QStringList m_placesOrder;

    // Suppresses order capture while the list is being repopulated: clear()
    // emits the same row signals a drag does, and capturing mid-rebuild would
    // save a half-empty order over the real one.
    bool m_rebuildingPlaces = false;

    // Guards updatePlacesHeight against recursing through the resize its own
    // setFixedHeight triggers.
    bool m_updatingPlacesHeight = false;

    QString m_currentDir;
};
