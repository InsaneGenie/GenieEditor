#include "KlipyPanel.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QMovie>
#include <QBuffer>
#include <QSettings>
#include <QInputDialog>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QDesktopServices>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QEventLoop>
#include <QGuiApplication>

namespace {
// Sized so two columns fit in a narrow side dock. Bigger tiles looked better in
// isolation but forced a single column, which halves how many GIFs you can scan
// at once — and scanning is the entire point of a results grid.
constexpr int kThumbW = 116;
constexpr int kThumbH = 88;

// How long a drag will wait for a GIF to finish downloading before giving up.
// Generous, because the alternative to waiting is the drag not happening at
// all — but bounded, because a nested event loop that never exits would take
// the window with it.
constexpr int kDragDownloadTimeoutMs = 15000;

// The one place the cache filename pattern is written down.
QString cacheFilePathFor(const QString& dir, const QString& id) {
    return QDir(dir).absoluteFilePath(QStringLiteral("klipy_%1.gif").arg(id));
}
} // namespace

KlipyPanel::KlipyPanel(QWidget* parent) : QWidget(parent) {
    m_client = new KlipyClient(this);
    m_client->setApiKey(QSettings().value("klipy/apiKey").toString());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_searchBox = new QLineEdit(this);
    // Klipy's attribution guidelines specifically require "Search KLIPY" as the
    // search field placeholder, so this string is a compliance requirement
    // rather than a wording choice.
    m_searchBox->setPlaceholderText("Search KLIPY");
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->addAction(Theme::icon(Theme::Icon::Search, Theme::textFaint(), 14),
                           QLineEdit::LeadingPosition);

    m_apiKeyButton = new QPushButton("Set API key\u2026", this);
    m_apiKeyButton->setProperty("variant", "primary");
    m_apiKeyButton->setCursor(Qt::PointingHandCursor);
    m_apiKeyButton->setToolTip("Klipy needs a free API key from the Klipy Partner Panel");
    connect(m_apiKeyButton, &QPushButton::clicked, this, &KlipyPanel::promptForApiKey);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(6);
    topRow->addWidget(m_searchBox, /*stretch=*/1);
    topRow->addWidget(m_apiKeyButton);
    root->addLayout(topRow);

    m_grid = new QListWidget(this);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setIconSize(QSize(kThumbW, kThumbH));
    // The grid cell has to clear the icon plus the view's own item margins, or
    // the items silently stop fitting side by side.
    m_grid->setGridSize(QSize(kThumbW + 16, kThumbH + 16));
    m_grid->setSpacing(2);
    m_grid->setWordWrap(false);
    m_grid->setMouseTracking(true); // needed for hover-to-animate
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    // Drag-out is driven by hand from eventFilter rather than by
    // setDragEnabled(true): the view's built-in drag would fire at the moment
    // the pointer moves, and at that moment there is no local file to put in
    // the payload — the GIF hasn't been downloaded yet.
    m_grid->viewport()->installEventFilter(this);
    root->addWidget(m_grid, /*stretch=*/1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    root->addWidget(m_status);

    // Klipy's terms require visible attribution wherever their results are
    // shown, so this is not decoration and shouldn't be removed.
    auto* attribution = new QLabel("Powered by KLIPY", this);
    attribution->setFont(Theme::uiFont(-2));
    attribution->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    attribution->setAlignment(Qt::AlignRight);
    root->addWidget(attribution);

    // Debounced so a search isn't fired for every keystroke — that would burn
    // through the API quota and mostly return results for prefixes nobody
    // wanted.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(350);
    connect(m_debounce, &QTimer::timeout, this, &KlipyPanel::runSearch);
    connect(m_searchBox, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_searchBox, &QLineEdit::returnPressed, this, [this] {
        m_debounce->stop();
        runSearch();
    });

    connect(m_grid, &QListWidget::itemActivated, this, &KlipyPanel::importItem);
    connect(m_grid, &QListWidget::itemDoubleClicked, this, &KlipyPanel::importItem);

    // Hover starts the animation for one item and stops the previous one.
    connect(m_grid, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        const QString id = item ? item->data(Qt::UserRole).toString() : QString();
        if (id == m_hoveredId) return;

        if (auto previous = m_previews.constFind(m_hoveredId); previous != m_previews.constEnd()) {
            if (previous->movie) previous->movie->setPaused(true);
        }
        m_hoveredId = id;
        if (auto current = m_previews.constFind(id); current != m_previews.constEnd()) {
            if (current->movie) current->movie->setPaused(false);
        }
    });

    connect(m_client, &KlipyClient::searchFinished, this, &KlipyPanel::showResults);
    connect(m_client, &KlipyClient::searchFailed, this, [this](const QString& message) {
        setStatus(message, /*isError=*/true);
    });

    connect(m_client, &KlipyClient::bytesFetched, this, [this](const QString& token, const QByteArray& data) {
        if (token.startsWith("preview:")) {
            const QString id = token.mid(8);

            // The bytes have to outlive the QMovie that reads them, hence the
            // QBuffer kept alongside it rather than a local.
            auto* buffer = new QBuffer(this);
            buffer->setData(data);
            buffer->open(QIODevice::ReadOnly);

            auto* movie = new QMovie(buffer, "gif", this);
            movie->setScaledSize(QSize(kThumbW, kThumbH));
            movie->setCacheMode(QMovie::CacheAll);

            PreviewMovie preview{movie, buffer};
            m_previews.insert(id, preview);

            // Jump to the first frame and stop there. Static until hovered.
            movie->jumpToFrame(0);

            connect(movie, &QMovie::frameChanged, this, [this, id] {
                for (int i = 0; i < m_grid->count(); ++i) {
                    QListWidgetItem* item = m_grid->item(i);
                    if (item->data(Qt::UserRole).toString() != id) continue;
                    auto found = m_previews.constFind(id);
                    if (found != m_previews.constEnd() && found->movie) {
                        item->setIcon(QIcon(found->movie->currentPixmap()));
                    }
                    break;
                }
            });

            for (int i = 0; i < m_grid->count(); ++i) {
                QListWidgetItem* item = m_grid->item(i);
                if (item->data(Qt::UserRole).toString() == id) {
                    item->setIcon(QIcon(movie->currentPixmap()));
                    break;
                }
            }
            return;
        }

        // "import:" (double-click) and "drag:" fetch the same bytes and save
        // them to the same place. They only differ in what happens next —
        // import hands the path straight to the timeline, drag hands it back to
        // whichever ensureLocalGif call is waiting on it.
        const bool isImport = token.startsWith("import:");
        const bool isDrag   = token.startsWith("drag:");
        if (isImport || isDrag) {
            const QString id = token.mid(isImport ? 7 : 5);
            QDir().mkpath(cacheDir());
            const QString path = cacheFilePathFor(cacheDir(), id);

            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) {
                setStatus("Couldn't save the GIF to the cache folder.", /*isError=*/true);
                if (isDrag && m_dragWaitId == id && m_dragWaitLoop) m_dragWaitLoop->quit();
                return;
            }
            file.write(data);
            file.close();

            if (isDrag) {
                if (m_dragWaitId == id && m_dragWaitLoop) {
                    m_dragWaitPath = path;
                    m_dragWaitLoop->quit();
                }
                return;
            }

            setStatus("Imported to the timeline.");
            emit gifReady(path);
        }
    });

    connect(m_client, &KlipyClient::bytesFailed, this, [this](const QString& token, const QString& message) {
        if (token.startsWith("import:")) setStatus("Download failed: " + message, /*isError=*/true);
        if (token.startsWith("drag:")) {
            setStatus("Download failed: " + message, /*isError=*/true);
            // Let the waiting drag give up now rather than sit out its timeout.
            if (m_dragWaitId == token.mid(5) && m_dragWaitLoop) m_dragWaitLoop->quit();
        }
        // A failed preview is left as a blank tile — one thumbnail that didn't
        // load isn't worth an error message over the whole grid.
    });

    updateApiKeyState();
}

KlipyPanel::~KlipyPanel() {
    // Stop every movie before the buffers they read from are torn down.
    for (auto& preview : m_previews) {
        if (preview.movie) preview.movie->stop();
    }
}

QString KlipyPanel::cacheDir() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .absoluteFilePath("klipy");
}

void KlipyPanel::setStatus(const QString& text, bool isError) {
    m_status->setText(text);
    m_status->setStyleSheet(QString("color: %1;")
        .arg(isError ? Theme::danger().name() : Theme::textFaint().name()));
}

void KlipyPanel::updateApiKeyState() {
    const bool ready = m_client->hasApiKey();
    m_apiKeyButton->setVisible(!ready);
    m_searchBox->setEnabled(ready);
    if (!ready) {
        setStatus("Searching KLIPY needs a free API key. Create one in the Klipy "
                  "Partner Panel at klipy.com, then paste it here. A test key allows "
                  "100 searches per hour.");
    } else if (m_grid->count() == 0) {
        setStatus("Type to search. Drag a GIF onto the timeline, or double-click to append it.");
    }
}

void KlipyPanel::promptForApiKey() {
    bool ok = false;
    const QString key = QInputDialog::getText(
        this, "KLIPY API Key",
        "Paste your KLIPY API key.\n\n"
        "Create one for free in the Partner Panel at klipy.com. A test key allows "
        "100 requests per hour; production access is requested from the same panel.",
        QLineEdit::Normal, m_client->apiKey(), &ok);
    if (!ok) return;

    m_client->setApiKey(key.trimmed());
    QSettings().setValue("klipy/apiKey", key.trimmed());
    updateApiKeyState();
    if (m_client->hasApiKey() && !m_searchBox->text().trimmed().isEmpty()) runSearch();
}

void KlipyPanel::runSearch() {
    if (!m_client->hasApiKey()) return;
    const QString query = m_searchBox->text().trimmed();
    if (query.isEmpty()) {
        m_grid->clear();
        setStatus("Type to search. Drag a GIF onto the timeline, or double-click to append it.");
        return;
    }
    setStatus(QString("Searching for \u201c%1\u201d\u2026").arg(query));
    m_client->search(query);
}

void KlipyPanel::showResults(const QVector<KlipyGif>& results) {
    // Previous previews are torn down before the grid is rebuilt, or their
    // frameChanged handlers would keep firing against items that no longer
    // exist.
    for (auto& preview : m_previews) {
        if (preview.movie) preview.movie->stop();
        delete preview.movie;
        delete preview.buffer;
    }
    m_previews.clear();
    m_hoveredId.clear();

    m_grid->clear();
    m_results = results;

    if (results.isEmpty()) {
        setStatus("No GIFs found for that search.");
        return;
    }
    setStatus(QString("%1 results \u2014 drag one onto the timeline, or double-click to append it.")
                  .arg(results.size()));

    for (const KlipyGif& gif : results) {
        auto* item = new QListWidgetItem(m_grid);
        item->setData(Qt::UserRole, gif.id);
        item->setToolTip(gif.description);
        item->setSizeHint(QSize(kThumbW, kThumbH));
        // A blank tile of the right size until the preview arrives, so the grid
        // doesn't reflow as thumbnails come in one by one.
        QPixmap placeholder(kThumbW, kThumbH);
        placeholder.fill(Theme::bg0());
        item->setIcon(QIcon(placeholder));

        if (!gif.previewUrl.isEmpty()) {
            m_client->fetchBytes(QUrl(gif.previewUrl), "preview:" + gif.id);
        }
    }
}

void KlipyPanel::importItem(QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();

    for (const KlipyGif& gif : m_results) {
        if (gif.id != id) continue;

        // Already downloaded once — reuse it rather than fetching again.
        const QString existing = cachedPathFor(id);
        if (!existing.isEmpty()) {
            setStatus("Imported to the timeline.");
            emit gifReady(existing);
            return;
        }

        setStatus("Downloading\u2026");
        m_client->fetchBytes(QUrl(gif.fullUrl), "import:" + id);
        return;
    }
}

// --- Drag-out ---------------------------------------------------------------
//
// The goal is that a GIF behaves like any other media file: drag it onto the
// timeline and it lands where you dropped it. Timeline::dropEvent already reads
// text/uri-list and MainWindow::onMediaDropped already routes a .gif to a video
// track, so nothing downstream needs to change — a GIF only has to become a
// local file with a URL before the drag starts.
//
// That "before" is the whole difficulty. At press time the GIF exists only as a
// Klipy URL, so the download is kicked off on press and, if it hasn't landed by
// the time the pointer has moved far enough to count as a drag, waited for.

bool KlipyPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_grid->viewport()) return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton) break;

        const QPoint pos = me->position().toPoint();
        QListWidgetItem* item = m_grid->itemAt(pos);
        m_pressPos = pos;
        m_pressId = item ? item->data(Qt::UserRole).toString() : QString();

        // Start the download NOW rather than waiting to see whether this turns
        // into a drag. Pressing costs one request that a plain click would have
        // made anyway a moment later, and it buys the time it takes to move the
        // pointer — usually enough that the drag starts with no wait at all.
        if (!m_pressId.isEmpty() && cachedPathFor(m_pressId).isEmpty()) {
            const QString url = fullUrlFor(m_pressId);
            if (!url.isEmpty()) m_client->fetchBytes(QUrl(url), "drag:" + m_pressId);
        }
        break; // fall through to the view so selection still works
    }

    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (m_pressId.isEmpty() || m_dragBusy) break;
        if (!(me->buttons() & Qt::LeftButton)) break;
        if ((me->position().toPoint() - m_pressPos).manhattanLength()
            < QApplication::startDragDistance()) {
            break;
        }

        const QString id = m_pressId;
        m_pressId.clear(); // one drag per press
        startDragFor(id);
        return true; // the drag owns this gesture now
    }

    case QEvent::MouseButtonRelease:
        m_pressId.clear();
        break;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

QString KlipyPanel::cachedPathFor(const QString& id) {
    if (id.isEmpty()) return QString();
    const QString path = cacheFilePathFor(cacheDir(), id);
    return QFile::exists(path) ? path : QString();
}

QString KlipyPanel::fullUrlFor(const QString& id) const {
    for (const KlipyGif& gif : m_results) {
        if (gif.id == id) return gif.fullUrl;
    }
    return QString();
}

QString KlipyPanel::ensureLocalGif(const QString& id) {
    const QString cached = cachedPathFor(id);
    if (!cached.isEmpty()) return cached;

    const QString url = fullUrlFor(id);
    if (url.isEmpty()) return QString();

    // The press handler normally has this in flight already. Re-requesting when
    // it doesn't (a drag that began some other way) is cheaper than tracking
    // in-flight requests, and KlipyClient tokens keep the responses apart.
    if (m_dragWaitId != id) m_client->fetchBytes(QUrl(url), "drag:" + id);

    setStatus("Downloading\u2026");

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    m_dragWaitId = id;
    m_dragWaitPath.clear();
    m_dragWaitLoop = &loop;

    QApplication::setOverrideCursor(Qt::BusyCursor);
    timeout.start(kDragDownloadTimeoutMs);
    // User input is deliberately NOT excluded here. Excluding it would leave the
    // mouse-release queued and undelivered, so the drag below would start
    // believing the button was still down and then drop the moment that stale
    // release arrived — landing the clip somewhere nobody chose.
    loop.exec();
    QApplication::restoreOverrideCursor();

    m_dragWaitLoop = nullptr;
    m_dragWaitId.clear();

    const QString path = m_dragWaitPath;
    m_dragWaitPath.clear();
    return path;
}

void KlipyPanel::startDragFor(const QString& id) {
    if (m_dragBusy) return;

    m_dragBusy = true;
    const QString path = ensureLocalGif(id); // may spin an event loop
    m_dragBusy = false;

    if (path.isEmpty()) return; // failed, timed out, or no longer in the results

    // A download that took a while may have outlasted the gesture. Starting a
    // QDrag with no button held gives Qt's "sticky" drag, which follows the
    // pointer until clicked — surprising enough that it's better to do nothing
    // and say the GIF is ready, since the second attempt is now instant.
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        setStatus("Ready \u2014 drag it onto the timeline.");
        return;
    }

    auto* mime = new QMimeData();
    mime->setUrls({QUrl::fromLocalFile(path)});

    auto* drag = new QDrag(this); // Qt deletes this when the drag finishes
    drag->setMimeData(mime);

    // Drag the frame currently showing in the grid, so what's under the cursor
    // is recognisably the GIF that was picked. Absent (preview never loaded, or
    // a newer search cleared it) is fine — Qt falls back to a default cursor.
    if (auto preview = m_previews.constFind(id); preview != m_previews.constEnd() && preview->movie) {
        const QPixmap frame = preview->movie->currentPixmap();
        if (!frame.isNull()) {
            drag->setPixmap(frame);
            drag->setHotSpot(QPoint(frame.width() / 2, frame.height() / 2));
        }
    }

    setStatus("Drop it on a video track.");
    drag->exec(Qt::CopyAction);
}
