#include "SoundEffectsPanel.h"
#include "Theme.h"
#include "AudioPlayer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QToolButton>
#include <QSlider>
#include <QSettings>
#include <QTimer>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
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
#include <QRegularExpression>

namespace {

// How long a drag will wait for a sound to finish downloading before giving up.
// Generous, because the alternative to waiting is the drag not happening at all
// — but bounded, because a nested event loop that never exits takes the window
// with it. Sound effects are small, so this is rarely approached.
constexpr int kDragDownloadTimeoutMs = 12000;

// The one place the cache filename pattern is written down.
QString cacheFilePathFor(const QString& dir, const QString& id) {
    // The id comes from a scraped page, so it can contain anything. Sanitised
    // before it becomes a filename rather than trusted — a slug with a slash in
    // it would otherwise write outside the cache directory.
    QString safe = id;
    safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    if (safe.isEmpty()) safe = "sound";
    return QDir(dir).absoluteFilePath(QStringLiteral("instant_%1.mp3").arg(safe));
}

} // namespace

SoundEffectsPanel::SoundEffectsPanel(QWidget* parent) : QWidget(parent) {
    m_client = new MyInstantsClient(this);
    m_preview = new AudioPlayer(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // --- Search row -------------------------------------------------------
    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText("Search sound effects\u2026");
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->addAction(Theme::icon(Theme::Icon::Search, Theme::textFaint(), 14),
                           QLineEdit::LeadingPosition);

    // The endpoint is a third-party service that can move or go down, so
    // changing it is a first-class action rather than something needing a
    // rebuild. See MyInstantsClient's header for why it isn't hardcoded.
    m_settingsButton = new QToolButton(this);
    m_settingsButton->setObjectName("keyframeButton");
    m_settingsButton->setIcon(Theme::icon(Theme::Icon::Folder, Theme::textDim(), 16));
    m_settingsButton->setFixedSize(30, 28);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setToolTip("Sound source options");
    connect(m_settingsButton, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        QAction* trending = menu.addAction("Show trending");
        QAction* recent = menu.addAction("Show recently uploaded");
        QAction* best = menu.addAction("Show all-time best");
        menu.addSeparator();
        QAction* endpoint = menu.addAction("Set service URL\u2026");
        menu.addSeparator();
        QAction* site = menu.addAction("Open myinstants.com");

        QAction* chosen = menu.exec(m_settingsButton->mapToGlobal(QPoint(0, m_settingsButton->height())));
        if (!chosen) return;

        if (chosen == trending) {
            setStatus("Loading trending sounds\u2026");
            m_client->browse(MyInstantsClient::Browse::Trending);
        } else if (chosen == recent) {
            setStatus("Loading recent sounds\u2026");
            m_client->browse(MyInstantsClient::Browse::Recent);
        } else if (chosen == best) {
            setStatus("Loading top sounds\u2026");
            m_client->browse(MyInstantsClient::Browse::Best);
        } else if (chosen == site) {
            QDesktopServices::openUrl(QUrl("https://www.myinstants.com/"));
        } else if (chosen == endpoint) {
            bool ok = false;
            const QString value = QInputDialog::getText(
                this, "Sound Service URL",
                "MyInstants has no official API, so this panel talks to a community\n"
                "service that re-serves the site as JSON. If it stops working you can\n"
                "point this at another instance, or one you host yourself.\n\n"
                "Base URL:",
                QLineEdit::Normal, MyInstantsClient::baseUrl(), &ok);
            if (ok) {
                MyInstantsClient::setBaseUrl(value.trimmed().isEmpty()
                                                 ? MyInstantsClient::defaultBaseUrl()
                                                 : value);
                setStatus("Service URL updated.");
                m_client->browse(MyInstantsClient::Browse::Trending);
            }
        }
    });

    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(5);
    searchRow->addWidget(m_searchBox, /*stretch=*/1);
    searchRow->addWidget(m_settingsButton);
    root->addLayout(searchRow);

    // --- Results ----------------------------------------------------------
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setAlternatingRowColors(false);
    m_list->setWordWrap(false);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setIconSize(QSize(16, 16));
    m_list->viewport()->installEventFilter(this);
    root->addWidget(m_list, /*stretch=*/1);

    // Single click previews, double click imports. Preview on single click
    // because auditioning is the thing you do most in a sound library — needing
    // a modifier or a separate play button to hear something would put friction
    // on the common case.
    connect(m_list, &QListWidget::itemClicked, this, &SoundEffectsPanel::previewItem);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &SoundEffectsPanel::importItem);

    // --- Preview level ----------------------------------------------------
    m_muteButton = new QToolButton(this);
    m_muteButton->setObjectName("keyframeButton");
    m_muteButton->setFixedSize(26, 22);
    m_muteButton->setCursor(Qt::PointingHandCursor);
    connect(m_muteButton, &QToolButton::clicked, this, &SoundEffectsPanel::togglePreviewMute);

    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(QSettings().value("soundEffects/previewVolume",
                                               kDefaultPreviewVolume).toInt());
    m_volumeSlider->setToolTip("Preview volume \u2014 auditioning only, and not "
                              "applied to sounds you add to the timeline");

    m_volumeLabel = new QLabel(this);
    m_volumeLabel->setFont(Theme::monoFont(-2));
    m_volumeLabel->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    m_volumeLabel->setFixedWidth(34);
    m_volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        // Any movement counts as leaving mute, so nudging the slider while
        // muted doesn't leave a silent player disagreeing with a non-zero
        // slider.
        if (value > 0) m_volumeBeforeMute = 0;
        QSettings().setValue("soundEffects/previewVolume", value);
        applyPreviewVolume(); // live, so dragging adjusts a sound already playing
    });

    auto* volumeRow = new QHBoxLayout();
    volumeRow->setSpacing(5);
    volumeRow->addWidget(m_muteButton);
    volumeRow->addWidget(m_volumeSlider, /*stretch=*/1);
    volumeRow->addWidget(m_volumeLabel);
    root->addLayout(volumeRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setFont(Theme::uiFont(-2));
    m_statusLabel->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    root->addWidget(m_statusLabel);

    // --- Wiring -----------------------------------------------------------
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(350);
    connect(m_debounce, &QTimer::timeout, this, &SoundEffectsPanel::runSearch);
    connect(m_searchBox, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_searchBox, &QLineEdit::returnPressed, this, [this] {
        m_debounce->stop();
        runSearch();
    });

    connect(m_client, &MyInstantsClient::listFinished, this, &SoundEffectsPanel::showResults);
    connect(m_client, &MyInstantsClient::listFailed, this, [this](const QString& message) {
        setStatus(message, /*isError=*/true);
    });

    connect(m_client, &MyInstantsClient::bytesFetched, this,
            [this](const QString& token, const QByteArray& data) {
        // "import:" (double-click) and "drag:" fetch the same bytes to the same
        // place. They differ only in what happens next — import hands the path
        // straight to the timeline, drag hands it back to whichever
        // ensureLocalSound call is waiting on it.
        const bool isImport = token.startsWith("import:");
        const bool isDrag = token.startsWith("drag:");
        if (!isImport && !isDrag) return;

        const QString id = token.mid(isImport ? 7 : 5);
        QDir().mkpath(cacheDir());
        const QString path = cacheFilePathFor(cacheDir(), id);

        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            setStatus("Couldn't save the sound to the cache folder.", /*isError=*/true);
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

        setStatus("Added to an audio track.");
        emit soundReady(path);
    });

    connect(m_client, &MyInstantsClient::bytesFailed, this,
            [this](const QString& token, const QString& message) {
        if (token.startsWith("import:") || token.startsWith("drag:")) {
            setStatus("Download failed: " + message, /*isError=*/true);
            if (token.startsWith("drag:") && m_dragWaitId == token.mid(5) && m_dragWaitLoop) {
                m_dragWaitLoop->quit(); // give up now rather than sitting out the timeout
            }
        }
    });

    applyPreviewVolume(); // sets the icon and readout to match the restored level

    setStatus("Loading trending sounds\u2026");
    m_client->browse(MyInstantsClient::Browse::Trending);
}

void SoundEffectsPanel::applyPreviewVolume() {
    const int volume = m_volumeSlider ? m_volumeSlider->value() : kDefaultPreviewVolume;
    if (m_preview) m_preview->setVolume(volume);

    if (m_volumeLabel) m_volumeLabel->setText(QString("%1%").arg(volume));
    if (m_muteButton) {
        m_muteButton->setIcon(Theme::icon(volume == 0 ? Theme::Icon::VolumeMute
                                                      : Theme::Icon::Volume,
                                          volume == 0 ? Theme::textFaint() : Theme::textDim(), 15));
        m_muteButton->setToolTip(volume == 0 ? "Unmute previews" : "Mute previews");
    }
}

void SoundEffectsPanel::togglePreviewMute() {
    if (!m_volumeSlider) return;

    if (m_volumeSlider->value() > 0) {
        // Remembered so unmuting returns to the level that was set rather than
        // to some default — otherwise muting quietly loses the setting.
        m_volumeBeforeMute = m_volumeSlider->value();
        m_volumeSlider->setValue(0);
    } else {
        const int restore = m_volumeBeforeMute > 0 ? m_volumeBeforeMute : kDefaultPreviewVolume;
        m_volumeBeforeMute = 0;
        m_volumeSlider->setValue(restore);
    }
}

SoundEffectsPanel::~SoundEffectsPanel() {
    if (m_dragWaitLoop) m_dragWaitLoop->quit(); // don't leave a nested loop running through teardown
    m_client->cancelAll();
}

QString SoundEffectsPanel::cacheDir() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .absoluteFilePath("sounds");
}

QString SoundEffectsPanel::cachedPathFor(const QString& id) {
    if (id.isEmpty()) return QString();
    const QString path = cacheFilePathFor(cacheDir(), id);
    return QFile::exists(path) ? path : QString();
}

InstantSound SoundEffectsPanel::soundFor(const QString& id) const {
    for (const InstantSound& sound : m_results) {
        if (sound.id == id) return sound;
    }
    return InstantSound();
}

void SoundEffectsPanel::setStatus(const QString& message, bool isError) {
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet(
        QString("color: %1;").arg((isError ? Theme::danger() : Theme::textFaint()).name()));
}

void SoundEffectsPanel::runSearch() {
    const QString query = m_searchBox->text().trimmed();
    if (query.isEmpty()) {
        setStatus("Loading trending sounds\u2026");
        m_client->browse(MyInstantsClient::Browse::Trending);
        return;
    }
    setStatus("Searching\u2026");
    m_client->search(query);
}

void SoundEffectsPanel::showResults(const QVector<InstantSound>& results) {
    stopPreview();
    m_list->clear();
    m_results = results;

    if (results.isEmpty()) {
        setStatus("No sounds found. Try a different search.");
        return;
    }

    for (const InstantSound& sound : results) {
        auto* item = new QListWidgetItem(
            Theme::icon(Theme::Icon::Volume, Theme::textDim(), 16), sound.title, m_list);
        item->setData(Qt::UserRole, sound.id);
        item->setToolTip(QString("%1\n\nClick to preview \u00b7 double-click to add at the playhead"
                                 "\n\u00b7 or drag onto an audio track")
                             .arg(sound.title));
    }

    setStatus(QString("%1 sounds \u2014 click to preview, drag onto an audio track, "
                      "or double-click to add.").arg(results.size()));
}

void SoundEffectsPanel::previewItem(QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();

    // Clicking the sound that's already playing stops it, so the same click
    // both starts and stops an audition.
    if (id == m_previewingId) {
        stopPreview();
        return;
    }

    const InstantSound sound = soundFor(id);
    if (sound.mp3Url.isEmpty()) return;

    // Streamed rather than downloaded. mpv opens a URL as readily as a file, and
    // waiting on a round trip before making any noise would make auditioning a
    // list of sounds tedious. A cached copy is preferred when there is one,
    // which makes re-previewing something already imported instant and offline.
    const QString cached = cachedPathFor(id);
    m_previewingId = id;
    m_preview->loadFile(cached.isEmpty() ? sound.mp3Url : cached);
    applyPreviewVolume(); // before play, so nothing is ever heard at the wrong level
    m_preview->play();
    setStatus(QString("Playing %1").arg(sound.title));
}

void SoundEffectsPanel::stopPreview() {
    if (m_previewingId.isEmpty()) return;
    m_preview->pause();
    m_previewingId.clear();
}

void SoundEffectsPanel::importItem(QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    const InstantSound sound = soundFor(id);
    if (sound.mp3Url.isEmpty()) return;

    // Already downloaded once — reuse it rather than fetching again.
    const QString existing = cachedPathFor(id);
    if (!existing.isEmpty()) {
        setStatus("Added to an audio track.");
        emit soundReady(existing);
        return;
    }

    m_pendingImportId = id;
    setStatus(QString("Downloading %1\u2026").arg(sound.title));
    m_client->fetchBytes(QUrl(sound.mp3Url), "import:" + id);
}

// --- Drag-out ---------------------------------------------------------------
//
// Identical in shape to the GIF panel's, and for the same reason: the timeline
// already accepts text/uri-list drops and already routes an audio file to an
// audio track, so a sound only has to become a local file with a URL before the
// drag starts. That "before" is the difficulty — at press time it exists only as
// a remote mp3.

bool SoundEffectsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_list->viewport()) return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton) break;

        const QPoint pos = me->position().toPoint();
        QListWidgetItem* item = m_list->itemAt(pos);
        m_pressPos = pos;
        m_pressId = item ? item->data(Qt::UserRole).toString() : QString();

        // Start the download NOW rather than waiting to see whether this becomes
        // a drag. Pressing costs one request that a double-click would have made
        // anyway, and it buys the time it takes to move the pointer.
        if (!m_pressId.isEmpty() && cachedPathFor(m_pressId).isEmpty()) {
            const InstantSound sound = soundFor(m_pressId);
            if (!sound.mp3Url.isEmpty()) {
                m_client->fetchBytes(QUrl(sound.mp3Url), "drag:" + m_pressId);
            }
        }
        break; // fall through so selection and preview still work
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

QString SoundEffectsPanel::ensureLocalSound(const QString& id) {
    const QString cached = cachedPathFor(id);
    if (!cached.isEmpty()) return cached;

    const InstantSound sound = soundFor(id);
    if (sound.mp3Url.isEmpty()) return QString();

    // The press handler normally has this in flight already. Re-requesting when
    // it doesn't is cheaper than tracking in-flight requests, and the tokens
    // keep the responses apart.
    if (m_dragWaitId != id) m_client->fetchBytes(QUrl(sound.mp3Url), "drag:" + id);

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
    // User input is deliberately NOT excluded. Excluding it would leave the
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

void SoundEffectsPanel::startDragFor(const QString& id) {
    if (m_dragBusy) return;

    m_dragBusy = true;
    const QString path = ensureLocalSound(id); // may spin an event loop
    m_dragBusy = false;

    if (path.isEmpty()) return; // failed, timed out, or no longer in the results

    // A download that took a while may have outlasted the gesture. Starting a
    // QDrag with no button held gives Qt's "sticky" drag, which follows the
    // pointer until clicked — surprising enough that it's better to do nothing
    // and say it's ready, since the second attempt is now instant.
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        setStatus("Ready \u2014 drag it onto an audio track.");
        return;
    }

    auto* mime = new QMimeData();
    mime->setUrls({QUrl::fromLocalFile(path)});

    auto* drag = new QDrag(this); // Qt deletes this when the drag finishes
    drag->setMimeData(mime);

    setStatus("Drop it on an audio track.");
    drag->exec(Qt::CopyAction);
}
