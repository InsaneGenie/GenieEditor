#include "MediaBrowserPanel.h"
#include "MediaThumbnailProxyModel.h"
#include "Theme.h"
#include <QToolButton>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <algorithm>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileSystemModel>
#include <QListView>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QFontMetrics>

MediaBrowserPanel::MediaBrowserPanel(QWidget* parent) : QWidget(parent) {
    m_model = new QFileSystemModel(this);
    // Show folders plus common media file types — anything else here would
    // just be clutter for what this panel exists to do.
    m_model->setNameFilters({"*.mp4", "*.mov", "*.mkv", "*.avi", "*.wav", "*.mp3", "*.m4a", "*.flac",
                              "*.png", "*.jpg", "*.jpeg", "*.bmp"});
    m_model->setNameFilterDisables(false); // hide non-matching files entirely rather than greying them out

    QString startDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (startDir.isEmpty()) startDir = QDir::homePath();
    m_model->setRootPath(startDir);

    m_proxyModel = new MediaThumbnailProxyModel(m_model, this);

    m_view = new QListView(this);
    m_view->setModel(m_proxyModel);

    // Icon-grid mode with real video thumbnails, similar to a media bin in
    // a real NLE, rather than a plain file-name list.
    m_view->setViewMode(QListView::IconMode);
    m_view->setIconSize(QSize(96, 54)); // 16:9, matches ThumbnailGenerator's default aspect
    m_view->setGridSize(QSize(112, 92));
    m_view->setResizeMode(QListView::Adjust);
    m_view->setWordWrap(true);
    m_view->setUniformItemSizes(true);
    m_view->setSpacing(4);

    // DragOnly — this panel is a source for dragging files out (onto
    // Timeline), not a destination for anything dropped onto it.
    m_view->setDragEnabled(true);
    m_view->setDragDropMode(QAbstractItemView::DragOnly);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_view, &QListView::doubleClicked, this, &MediaBrowserPanel::onItemDoubleClicked);

    m_view->setIconSize(QSize(104, 58));
    m_view->setGridSize(QSize(124, 104));

    // Icon-only, sized to match the row height of the path field beside it —
    // the old "⬆ Up" / "Choose Folder…" pair stretched across the full panel
    // width and dominated a panel whose actual job is showing thumbnails.
    m_upButton = new QPushButton(this);
    m_upButton->setIcon(Theme::icon(Theme::Icon::FolderUp, Theme::textDim(), 16));
    m_upButton->setIconSize(QSize(16, 16));
    m_upButton->setToolTip("Go to the parent folder");
    m_upButton->setFixedSize(30, 28);
    m_upButton->setCursor(Qt::PointingHandCursor);
    connect(m_upButton, &QPushButton::clicked, this, &MediaBrowserPanel::onUpClicked);

    auto* chooseFolderBtn = new QPushButton(this);
    chooseFolderBtn->setIcon(Theme::icon(Theme::Icon::Folder, Theme::textDim(), 16));
    chooseFolderBtn->setIconSize(QSize(16, 16));
    chooseFolderBtn->setToolTip("Browse for a different media folder");
    chooseFolderBtn->setFixedSize(30, 28);
    chooseFolderBtn->setCursor(Qt::PointingHandCursor);
    connect(chooseFolderBtn, &QPushButton::clicked, this, &MediaBrowserPanel::onChooseFolderClicked);

    // The current folder now sits on the same row as its controls, styled as a
    // path field rather than as loose grey text floating under the buttons.
    m_pathLabel = new QLabel(this);
    m_pathLabel->setFont(Theme::monoFont(-2));
    m_pathLabel->setStyleSheet(
        QString("color: %1; background: %2; border: 1px solid %3; border-radius: %4px; padding: 5px 9px;")
            .arg(Theme::textDim().name(), Theme::bg0().name(), Theme::line().name())
            .arg(Theme::kRadiusMd));
    m_pathLabel->setFixedHeight(28);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(5);
    buttonRow->addWidget(m_upButton);
    buttonRow->addWidget(m_pathLabel, /*stretch=*/1);
    buttonRow->addWidget(chooseFolderBtn);

    // Pin toggle sits with the other navigation controls, since pinning is
    // something you do TO the folder you're currently looking at.
    m_pinButton = new QToolButton(this);
    m_pinButton->setObjectName("keyframeButton"); // reuses the zero-padding rule for square icon buttons
    m_pinButton->setIconSize(QSize(16, 16));
    m_pinButton->setFixedSize(30, 28);
    m_pinButton->setCursor(Qt::PointingHandCursor);
    connect(m_pinButton, &QToolButton::clicked, this, &MediaBrowserPanel::onPinCurrentClicked);
    buttonRow->addWidget(m_pinButton);

    // --- Places ---------------------------------------------------------
    m_placesHeader = new QToolButton(this);
    m_placesHeader->setObjectName("placesHeader");
    m_placesHeader->setCheckable(true);
    m_placesHeader->setChecked(true);
    m_placesHeader->setText("PLACES");
    m_placesHeader->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_placesHeader->setIconSize(QSize(12, 12));
    m_placesHeader->setCursor(Qt::PointingHandCursor);
    QFont headerFont = Theme::uiFont(-2, QFont::DemiBold);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    m_placesHeader->setFont(headerFont);

    m_placesList = new QListWidget(this);
    m_placesList->setIconSize(QSize(16, 16));
    m_placesList->setUniformItemSizes(true);
    m_placesList->setContextMenuPolicy(Qt::CustomContextMenu);
    // Sized to show a handful of entries and scroll beyond that. The file grid
    // is the reason this panel exists, so the shortcut list is not allowed to
    // grow without bound and crowd it out.
    m_placesList->setMaximumHeight(168);
    connect(m_placesList, &QListWidget::itemClicked, this, &MediaBrowserPanel::onPlaceActivated);
    connect(m_placesList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_placesList->itemAt(pos);
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        if (!m_pinnedPaths.contains(path)) return; // standard OS folders aren't removable

        QMenu menu(this);
        QAction* unpin = menu.addAction("Remove from Places");
        if (menu.exec(m_placesList->mapToGlobal(pos)) == unpin) {
            m_pinnedPaths.removeAll(path);
            savePinnedPaths();
            rebuildPlacesList();
            updatePinButton();
        }
    });

    connect(m_placesHeader, &QToolButton::toggled, this, [this](bool expanded) {
        m_placesList->setVisible(expanded);
        m_placesHeader->setIcon(Theme::icon(
            expanded ? Theme::Icon::ChevronDown : Theme::Icon::ChevronRight, Theme::textFaint(), 12));
        QSettings().setValue("mediaBrowser/placesExpanded", expanded);
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addLayout(buttonRow);
    layout->addWidget(m_placesHeader);
    layout->addWidget(m_placesList);
    layout->addWidget(m_view, /*stretch=*/1);

    loadPinnedPaths();
    rebuildPlacesList();
    m_placesHeader->setChecked(QSettings().value("mediaBrowser/placesExpanded", true).toBool());
    // Force the icon/visibility to match even when setChecked didn't change it.
    m_placesList->setVisible(m_placesHeader->isChecked());
    m_placesHeader->setIcon(Theme::icon(
        m_placesHeader->isChecked() ? Theme::Icon::ChevronDown : Theme::Icon::ChevronRight,
        Theme::textFaint(), 12));

    navigateTo(startDir);
}

QVector<QPair<QString, QString>> MediaBrowserPanel::standardPlaces() {
    // Deliberately the OS's own idea of these folders rather than hardcoded
    // paths: they're relocatable, and on this machine several of them live
    // under OneDrive rather than the user profile.
    const QVector<QPair<QString, QStandardPaths::StandardLocation>> candidates = {
        {"Desktop",   QStandardPaths::DesktopLocation},
        {"Downloads", QStandardPaths::DownloadLocation},
        {"Documents", QStandardPaths::DocumentsLocation},
        {"Pictures",  QStandardPaths::PicturesLocation},
        {"Music",     QStandardPaths::MusicLocation},
        {"Videos",    QStandardPaths::MoviesLocation},
    };

    QVector<QPair<QString, QString>> places;
    for (const auto& candidate : candidates) {
        const QString path = QStandardPaths::writableLocation(candidate.second);
        if (!path.isEmpty() && QDir(path).exists()) places.push_back({candidate.first, path});
    }
    return places;
}

void MediaBrowserPanel::loadPinnedPaths() {
    m_pinnedPaths = QSettings().value("mediaBrowser/pinnedPaths").toStringList();
    // Drop anything that has since been deleted or unplugged, so the list can't
    // silently accumulate dead entries that do nothing when clicked.
    m_pinnedPaths.erase(std::remove_if(m_pinnedPaths.begin(), m_pinnedPaths.end(),
                                       [](const QString& path) { return !QDir(path).exists(); }),
                        m_pinnedPaths.end());
}

void MediaBrowserPanel::savePinnedPaths() const {
    QSettings().setValue("mediaBrowser/pinnedPaths", m_pinnedPaths);
}

void MediaBrowserPanel::rebuildPlacesList() {
    m_placesList->clear();

    auto addItem = [this](const QString& name, const QString& path, bool pinned) {
        auto* item = new QListWidgetItem(
            Theme::icon(pinned ? Theme::Icon::PinFilled : Theme::Icon::Folder,
                        pinned ? Theme::accent() : Theme::textDim(), 16),
            name, m_placesList);
        item->setData(Qt::UserRole, path);
        item->setToolTip(pinned ? QString("%1\n\nRight-click to remove from Places").arg(path) : path);
    };

    for (const auto& place : standardPlaces()) addItem(place.first, place.second, /*pinned=*/false);

    for (const QString& path : m_pinnedPaths) {
        // Trailing separators would otherwise make a drive root display as empty.
        QString name = QDir(path).dirName();
        if (name.isEmpty()) name = path;
        addItem(name, path, /*pinned=*/true);
    }
}

void MediaBrowserPanel::updatePinButton() {
    const bool pinned = m_pinnedPaths.contains(m_currentDir);
    m_pinButton->setIcon(Theme::icon(pinned ? Theme::Icon::PinFilled : Theme::Icon::Pin,
                                     pinned ? Theme::accent() : Theme::textDim(), 16));
    m_pinButton->setToolTip(pinned ? "Remove this folder from Places"
                                   : "Pin this folder to Places");

    // A standard folder is already in the list; offering to pin it again would
    // just produce a duplicate entry.
    bool isStandard = false;
    for (const auto& place : standardPlaces()) {
        if (place.second == m_currentDir) { isStandard = true; break; }
    }
    m_pinButton->setEnabled(!isStandard && !m_currentDir.isEmpty());
}

void MediaBrowserPanel::onPinCurrentClicked() {
    if (m_currentDir.isEmpty()) return;

    if (m_pinnedPaths.contains(m_currentDir)) m_pinnedPaths.removeAll(m_currentDir);
    else m_pinnedPaths.push_back(m_currentDir);

    savePinnedPaths();
    rebuildPlacesList();
    updatePinButton();
}

void MediaBrowserPanel::onPlaceActivated(QListWidgetItem* item) {
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty() || !QDir(path).exists()) return;

    // setRootPath as well as navigating: QFileSystemModel only watches and
    // populates beneath its root, so jumping outside the current tree without
    // this would show an empty folder.
    m_model->setRootPath(path);
    navigateTo(path);
}

void MediaBrowserPanel::navigateTo(const QString& dirPath) {
    m_view->setRootIndex(m_proxyModel->mapFromSource(m_model->index(dirPath)));

    // Elide the path to the label's available width rather than letting a
    // long path overflow the (typically narrow) side panel.
    const QFontMetrics fm(m_pathLabel->font());
    const int avail = (m_pathLabel->width() > 40 ? m_pathLabel->width() : 220) - 18; // minus the field's padding
    m_pathLabel->setText(fm.elidedText(dirPath, Qt::ElideMiddle, avail));
    m_pathLabel->setToolTip(dirPath);

    m_currentDir = dirPath;
    updatePinButton();

    // Highlight the Places entry matching where we now are, so the list doubles
    // as an indicator of where you are rather than just a set of jump targets.
    for (int i = 0; i < m_placesList->count(); ++i) {
        QListWidgetItem* item = m_placesList->item(i);
        const bool here = item->data(Qt::UserRole).toString() == dirPath;
        item->setSelected(here);
    }
}

void MediaBrowserPanel::onChooseFolderClicked() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Choose Media Folder", m_model->rootPath());
    if (dir.isEmpty()) return;
    m_model->setRootPath(dir);
    navigateTo(dir);
}

void MediaBrowserPanel::onUpClicked() {
    const QModelIndex currentRootSource = m_proxyModel->mapToSource(m_view->rootIndex());
    const QModelIndex parentSource = currentRootSource.parent();
    if (!parentSource.isValid()) return; // already at a filesystem root
    navigateTo(m_model->filePath(parentSource));
}

void MediaBrowserPanel::onItemDoubleClicked(const QModelIndex& index) {
    const QModelIndex sourceIndex = m_proxyModel->mapToSource(index);
    if (m_model->isDir(sourceIndex)) {
        // Navigate INTO the folder — previously this was a dead click that
        // did nothing at all.
        navigateTo(m_model->filePath(sourceIndex));
        return;
    }
    emit fileActivated(m_model->filePath(sourceIndex));
}
