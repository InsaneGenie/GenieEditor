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
#include <QLineEdit>
#include <QCompleter>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QKeyEvent>
#include <QTimer>
#include <QAbstractItemModel>

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

    // The current folder sits on the same row as its controls, and is EDITABLE:
    // pasting or typing a path is the fastest way to reach a folder you already
    // know the name of, and it was previously a dead label that could only be
    // changed by clicking through a dialog.
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setFont(Theme::monoFont(-2));
    m_pathEdit->setStyleSheet(
        QString("color: %1; background: %2; border: 1px solid %3; border-radius: %4px; padding: 5px 9px;")
            .arg(Theme::textDim().name(), Theme::bg0().name(), Theme::line().name())
            .arg(Theme::kRadiusMd));
    m_pathEdit->setFixedHeight(28);
    m_pathEdit->setPlaceholderText("Type or paste a folder path\u2026");
    m_pathEdit->setToolTip("Type a folder path and press Enter. Esc cancels.");
    m_pathEdit->setClearButtonEnabled(false); // a clear button here would only ever empty a path

    // Directory completion as you type. A SEPARATE QFileSystemModel from the
    // browser's own: this one has no name filters and lists directories across
    // the whole filesystem, whereas the browser's is scoped to a root and
    // filtered down to media files — completing against that would suggest
    // almost nothing.
    auto* completerModel = new QFileSystemModel(this);
    completerModel->setRootPath(QString());
    completerModel->setFilter(QDir::Dirs | QDir::Drives | QDir::NoDotAndDotDot);
    auto* pathCompleter = new QCompleter(completerModel, this);
    pathCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    pathCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_pathEdit->setCompleter(pathCompleter);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &MediaBrowserPanel::onPathEntered);
    m_pathEdit->installEventFilter(this); // Esc abandons the edit

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(5);
    buttonRow->addWidget(m_upButton);
    buttonRow->addWidget(m_pathEdit, /*stretch=*/1);
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
    m_placesList->setContextMenuPolicy(Qt::CustomContextMenu);

    // Laid out as a wrapping row of chips rather than a vertical list. Folder
    // names are short, so a vertical list spent the panel's whole width on one
    // name and its height on entries that could have sat side by side — in a
    // narrow side panel that height is the scarcest thing there is, and it was
    // being taken from the thumbnail grid this panel exists to show.
    m_placesList->setFlow(QListView::LeftToRight);
    m_placesList->setWrapping(true);
    m_placesList->setResizeMode(QListView::Adjust); // re-wrap when the panel is resized
    // Each chip is as wide as its own name. Uniform sizing would pad every
    // entry out to the widest one, which is the opposite of the point.
    m_placesList->setUniformItemSizes(false);
    m_placesList->setSpacing(3);
    m_placesList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_placesList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_placesList->setStyleSheet(
        QString("QListWidget { background: transparent; border: none; }"
                "QListWidget::item { padding: 3px 8px; border-radius: %1px; }")
            .arg(Theme::kRadiusSm));

    // Drag to reorder. InternalMove rather than a custom drag implementation
    // because the list is the whole model here — there's nowhere else for an
    // entry to go, and QListWidget already carries every item role across the
    // move, including the path in UserRole that everything else keys off.
    m_placesList->setDragDropMode(QAbstractItemView::InternalMove);
    m_placesList->setDefaultDropAction(Qt::MoveAction);
    m_placesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_placesList->installEventFilter(this); // Delete key removes an entry

    // Both signals, because a QListWidget internal move is not necessarily
    // reported as a move: depending on the drop it can arrive as an insert
    // followed by a remove. Deferring to the next event-loop turn means the
    // order is read once the model has finished settling either way.
    const auto onOrderMaybeChanged = [this] {
        QTimer::singleShot(0, this, [this] { capturePlacesOrder(); });
    };
    connect(m_placesList->model(), &QAbstractItemModel::rowsMoved, this, onOrderMaybeChanged);
    connect(m_placesList->model(), &QAbstractItemModel::rowsRemoved, this, onOrderMaybeChanged);
    // Height follows the number of wrapped rows rather than being fixed, so a
    // handful of folders takes one line instead of reserving space for a list
    // that isn't there. Capped at kMaxPlacesRows — the file grid is the reason
    // this panel exists, and the shortcut row is not allowed to crowd it out.
    m_placesList->installEventFilter(this); // also catches resize, to re-wrap
    updatePlacesHeight();
    connect(m_placesList, &QListWidget::itemClicked, this, &MediaBrowserPanel::onPlaceActivated);
    connect(m_placesList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_placesList->itemAt(pos);
        QMenu menu(this);

        // ANY entry can be removed now, standard OS folders included. Someone
        // who never puts footage in Music shouldn't have to keep looking at it,
        // and removePlace remembers the two cases differently so both stick.
        QAction* remove = nullptr;
        if (item) {
            remove = menu.addAction("Remove from Places");
            menu.addSeparator();
        }

        // Removal has to be reversible or it's a trap — the standard folders
        // can't be pinned back via the pin button, which refuses them.
        QAction* restore = menu.addAction("Restore removed default folders");
        restore->setEnabled(!m_hiddenPlaces.isEmpty());

        QAction* chosen = menu.exec(m_placesList->mapToGlobal(pos));
        if (chosen && chosen == remove) {
            removePlace(item->data(Qt::UserRole).toString());
        } else if (chosen == restore) {
            m_hiddenPlaces.clear();
            savePlacesConfig();
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

    loadPlacesConfig();
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

void MediaBrowserPanel::loadPlacesConfig() {
    QSettings settings;
    m_pinnedPaths = settings.value("mediaBrowser/pinnedPaths").toStringList();
    m_hiddenPlaces = settings.value("mediaBrowser/hiddenPlaces").toStringList();
    m_placesOrder = settings.value("mediaBrowser/placesOrder").toStringList();

    // Drop anything that has since been deleted or unplugged, so the list can't
    // silently accumulate dead entries that do nothing when clicked.
    m_pinnedPaths.erase(std::remove_if(m_pinnedPaths.begin(), m_pinnedPaths.end(),
                                       [](const QString& path) { return !QDir(path).exists(); }),
                        m_pinnedPaths.end());

    // The order list is deliberately NOT pruned against existence. It's only a
    // sort key, and a folder on a drive that happens to be unplugged today
    // should return to its old position when it comes back rather than jumping
    // to the end.
}

void MediaBrowserPanel::savePlacesConfig() const {
    QSettings settings;
    settings.setValue("mediaBrowser/pinnedPaths", m_pinnedPaths);
    settings.setValue("mediaBrowser/hiddenPlaces", m_hiddenPlaces);
    settings.setValue("mediaBrowser/placesOrder", m_placesOrder);
}

void MediaBrowserPanel::rebuildPlacesList() {
    // Guards capturePlacesOrder against the row signals clear() and the inserts
    // below emit — they look exactly like a drag to the model, and letting them
    // through would save a partial order over the real one.
    m_rebuildingPlaces = true;
    m_placesList->clear();

    struct Entry {
        QString name;
        QString path;
        bool pinned = false;
    };
    QVector<Entry> entries;

    for (const auto& place : standardPlaces()) {
        if (m_hiddenPlaces.contains(place.second)) continue; // removed by the user
        entries.push_back({place.first, place.second, /*pinned=*/false});
    }

    for (const QString& path : m_pinnedPaths) {
        // Trailing separators would otherwise make a drive root display as empty.
        QString name = QDir(path).dirName();
        if (name.isEmpty()) name = path;
        entries.push_back({name, path, /*pinned=*/true});
    }

    // Apply the user's own order. Both kinds of entry sort through the same
    // list, so a pinned folder can be dragged above Documents and stay there —
    // keeping the two groups separate would have made half the drops impossible
    // to honour. Anything the order doesn't mention (a folder pinned since it
    // was last saved) sorts to the end, in the order built above.
    const int unknown = m_placesOrder.size();
    std::stable_sort(entries.begin(), entries.end(),
                     [this, unknown](const Entry& a, const Entry& b) {
        const int ia = m_placesOrder.indexOf(a.path);
        const int ib = m_placesOrder.indexOf(b.path);
        return (ia < 0 ? unknown : ia) < (ib < 0 ? unknown : ib);
    });

    for (const Entry& entry : entries) {
        auto* item = new QListWidgetItem(
            Theme::icon(entry.pinned ? Theme::Icon::PinFilled : Theme::Icon::Folder,
                        entry.pinned ? Theme::accent() : Theme::textDim(), 16),
            entry.name, m_placesList);
        item->setData(Qt::UserRole, entry.path);
        item->setToolTip(QString("%1\n\nDrag to reorder \u00b7 Delete or right-click to remove")
                             .arg(entry.path));
        // Dropping ONTO an entry would make it a container, which it isn't —
        // only dropping BETWEEN entries reorders.
        item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    }

    m_rebuildingPlaces = false;
    updatePlacesHeight(); // the chip count just changed, so the row count may have

    // Fold the freshly built order back in, so a first run (or a newly pinned
    // folder) has a recorded position instead of relying on the fallback every
    // time it rebuilds.
    capturePlacesOrder();
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

    savePlacesConfig();
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

    // The full path, unelided — it's an editable field now, so it scrolls
    // rather than needing to be shortened, and a truncated path would be
    // useless the moment you tried to edit it. Home is moved to the front so a
    // long path shows its most specific part rather than its drive letter.
    m_pathEdit->setText(dirPath);
    m_pathEdit->setCursorPosition(dirPath.size());
    m_pathEdit->setToolTip(dirPath);

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

void MediaBrowserPanel::onPathEntered() {
    QString typed = m_pathEdit->text().trimmed();
    if (typed.isEmpty()) { flashPathError(); return; }

    // Strip a wrapping pair of quotes: "Copy as path" in Windows Explorer adds
    // them, and pasting that in is one of the main reasons to have this field.
    if (typed.size() >= 2 && typed.startsWith('"') && typed.endsWith('"')) {
        typed = typed.mid(1, typed.size() - 2);
    }
    // ~ and ~/... expand to the home folder. Qt does not do this for us, and a
    // literal ~ folder is not a thing anyone means.
    if (typed == "~") {
        typed = QDir::homePath();
    } else if (typed.startsWith("~/") || typed.startsWith("~\\")) {
        typed = QDir::homePath() + "/" + typed.mid(2);
    }
    // Accepts either separator regardless of platform — a Windows path pasted
    // with backslashes has to work, and cleanPath normalises them.
    typed = QDir::cleanPath(typed.replace('\\', '/'));

    // Relative paths resolve against the folder currently shown, so "../raw" or
    // a bare subfolder name does what it looks like it should.
    QFileInfo info(typed);
    if (info.isRelative() && !m_currentDir.isEmpty()) {
        info = QFileInfo(QDir(m_currentDir).absoluteFilePath(typed));
    }

    // Typing a FILE path navigates to the folder containing it rather than
    // failing — that's what someone dragging a path out of another window
    // means, and refusing it would be needlessly literal.
    QString target = info.absoluteFilePath();
    if (info.exists() && !info.isDir()) target = info.absolutePath();

    if (target.isEmpty() || !QDir(target).exists()) {
        flashPathError();
        return;
    }

    // setRootPath as well as navigating, for the same reason onPlaceActivated
    // does it: the model only populates beneath its root, so jumping outside
    // the current tree without this shows an empty folder.
    m_model->setRootPath(target);
    navigateTo(target);
    m_view->setFocus(Qt::OtherFocusReason); // the field has done its job
}

void MediaBrowserPanel::flashPathError() {
    const QString base =
        QString("color: %1; background: %2; border: 1px solid %3; border-radius: %4px; padding: 5px 9px;");
    m_pathEdit->setStyleSheet(base.arg(Theme::text().name(), Theme::bg0().name(),
                                       Theme::danger().name()).arg(Theme::kRadiusMd));

    // Restore the border AND the path actually being shown. Leaving the bad
    // text in place would make the field disagree with the grid below it.
    QTimer::singleShot(900, this, [this, base] {
        m_pathEdit->setStyleSheet(base.arg(Theme::textDim().name(), Theme::bg0().name(),
                                           Theme::line().name()).arg(Theme::kRadiusMd));
        if (!m_currentDir.isEmpty()) m_pathEdit->setText(m_currentDir);
    });
}

void MediaBrowserPanel::removePlace(const QString& path) {
    if (path.isEmpty()) return;

    if (m_pinnedPaths.contains(path)) {
        m_pinnedPaths.removeAll(path);
    } else {
        // A standard OS folder: remembering it as hidden is the only way the
        // removal survives, since standardPlaces() regenerates from the OS.
        if (!m_hiddenPlaces.contains(path)) m_hiddenPlaces.push_back(path);
    }
    m_placesOrder.removeAll(path);

    savePlacesConfig();
    rebuildPlacesList();
    updatePinButton();
}

void MediaBrowserPanel::capturePlacesOrder() {
    if (m_rebuildingPlaces) return;

    QStringList order;
    for (int i = 0; i < m_placesList->count(); ++i) {
        const QString path = m_placesList->item(i)->data(Qt::UserRole).toString();
        if (!path.isEmpty()) order.push_back(path);
    }
    // A drag is briefly mid-flight when the source row has been removed and the
    // destination not yet inserted. Saving then would drop an entry, so a list
    // that got shorter than what it represents is ignored and the next signal
    // (the one after the move completes) captures the real order.
    if (order.size() < m_placesOrder.size()) return;

    m_placesOrder = order;
    savePlacesConfig();
}

bool MediaBrowserPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_placesList && event->type() == QEvent::Resize) {
        // Narrowing the panel re-wraps the chips into more rows, which changes
        // the height they need. Deferred, because the viewport width isn't
        // updated until after this event has been delivered — measuring now
        // would pack against the width it had a moment ago.
        QTimer::singleShot(0, this, [this] { updatePlacesHeight(); });
        return false;
    }
    if (watched == m_placesList && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            if (QListWidgetItem* item = m_placesList->currentItem()) {
                removePlace(item->data(Qt::UserRole).toString());
                return true;
            }
        }
    }
    if (watched == m_pathEdit && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            // Abandon the edit and put back the folder actually on screen,
            // rather than leaving a half-typed path sitting in the field.
            m_pathEdit->setText(m_currentDir);
            m_view->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MediaBrowserPanel::updatePlacesHeight() {
    if (!m_placesList || m_updatingPlacesHeight) return;

    // Changing the height fires a resize, which asks for the height again.
    // Without this guard that recurses.
    m_updatingPlacesHeight = true;

    const int count = m_placesList->count();
    if (count == 0) {
        m_placesList->setFixedHeight(0);
        m_updatingPlacesHeight = false;
        return;
    }

    const int spacing = m_placesList->spacing();

    // Measured with the scrollbar OFF. Leaving it on creates a feedback loop:
    // the scrollbar narrows the viewport, the narrower viewport pushes a chip
    // onto another row, and that extra row justifies the scrollbar that caused
    // it — leaving a scrollbar permanently attached to a row that fits.
    m_placesList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Qt's own laid-out rectangles rather than a re-implementation of its
    // wrapping. Predicting where the delegate breaks rows means duplicating
    // logic that already ran, and being a few pixels out shows up as a clipped
    // final row.
    int contentBottom = 0;
    int rowHeight = 0;
    for (int i = 0; i < count; ++i) {
        const QRect itemRect = m_placesList->visualItemRect(m_placesList->item(i));
        if (itemRect.height() <= 0) { contentBottom = 0; break; } // not laid out yet
        contentBottom = std::max(contentBottom, itemRect.bottom() + 1);
        rowHeight = std::max(rowHeight, itemRect.height() + 2 * spacing);
    }

    if (rowHeight <= 0) rowHeight = m_placesList->iconSize().height() + 12;

    // Before the widget has ever been shown there is no layout to measure, so
    // fall back to one row's worth. The deferred call after the next resize
    // replaces it with the real figure.
    if (contentBottom <= 0) contentBottom = rowHeight;

    const int wanted = contentBottom + spacing + 2 * m_placesList->frameWidth();
    const int cap = kMaxPlacesRows * rowHeight + 2 * m_placesList->frameWidth();

    if (wanted > cap) {
        // Genuinely more rows than the cap allows — now a scrollbar is correct.
        m_placesList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_placesList->setFixedHeight(cap);
    } else {
        m_placesList->setFixedHeight(wanted);
    }

    m_updatingPlacesHeight = false;
}
