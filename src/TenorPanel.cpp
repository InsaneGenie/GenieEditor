#include "TenorPanel.h"
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

namespace {
// Sized so two columns fit in a narrow side dock. Bigger tiles looked better in
// isolation but forced a single column, which halves how many GIFs you can scan
// at once — and scanning is the entire point of a results grid.
constexpr int kThumbW = 116;
constexpr int kThumbH = 88;
} // namespace

TenorPanel::TenorPanel(QWidget* parent) : QWidget(parent) {
    m_client = new TenorClient(this);
    m_client->setApiKey(QSettings().value("tenor/apiKey").toString());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText("Search Tenor");
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->addAction(Theme::icon(Theme::Icon::Search, Theme::textFaint(), 14),
                           QLineEdit::LeadingPosition);

    m_apiKeyButton = new QPushButton("Set API key\u2026", this);
    m_apiKeyButton->setProperty("variant", "primary");
    m_apiKeyButton->setCursor(Qt::PointingHandCursor);
    m_apiKeyButton->setToolTip("Tenor needs a free API key from Google Cloud");
    connect(m_apiKeyButton, &QPushButton::clicked, this, &TenorPanel::promptForApiKey);

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
    root->addWidget(m_grid, /*stretch=*/1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    root->addWidget(m_status);

    // Tenor's terms require visible attribution wherever their results are
    // shown, so this is not decoration and shouldn't be removed.
    auto* attribution = new QLabel("Powered by Tenor", this);
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
    connect(m_debounce, &QTimer::timeout, this, &TenorPanel::runSearch);
    connect(m_searchBox, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_searchBox, &QLineEdit::returnPressed, this, [this] {
        m_debounce->stop();
        runSearch();
    });

    connect(m_grid, &QListWidget::itemActivated, this, &TenorPanel::importItem);
    connect(m_grid, &QListWidget::itemDoubleClicked, this, &TenorPanel::importItem);

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

    connect(m_client, &TenorClient::searchFinished, this, &TenorPanel::showResults);
    connect(m_client, &TenorClient::searchFailed, this, [this](const QString& message) {
        setStatus(message, /*isError=*/true);
    });

    connect(m_client, &TenorClient::bytesFetched, this, [this](const QString& token, const QByteArray& data) {
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

        if (token.startsWith("import:")) {
            const QString id = token.mid(7);
            QDir().mkpath(cacheDir());
            const QString path = QDir(cacheDir()).absoluteFilePath(QString("tenor_%1.gif").arg(id));

            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) {
                setStatus("Couldn't save the GIF to the cache folder.", /*isError=*/true);
                return;
            }
            file.write(data);
            file.close();

            setStatus("Imported to the timeline.");
            emit gifReady(path);
        }
    });

    connect(m_client, &TenorClient::bytesFailed, this, [this](const QString& token, const QString& message) {
        if (token.startsWith("import:")) setStatus("Download failed: " + message, /*isError=*/true);
        // A failed preview is left as a blank tile — one thumbnail that didn't
        // load isn't worth an error message over the whole grid.
    });

    updateApiKeyState();
}

TenorPanel::~TenorPanel() {
    // Stop every movie before the buffers they read from are torn down.
    for (auto& preview : m_previews) {
        if (preview.movie) preview.movie->stop();
    }
}

QString TenorPanel::cacheDir() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .absoluteFilePath("tenor");
}

void TenorPanel::setStatus(const QString& text, bool isError) {
    m_status->setText(text);
    m_status->setStyleSheet(QString("color: %1;")
        .arg(isError ? Theme::danger().name() : Theme::textFaint().name()));
}

void TenorPanel::updateApiKeyState() {
    const bool ready = m_client->hasApiKey();
    m_apiKeyButton->setVisible(!ready);
    m_searchBox->setEnabled(ready);
    if (!ready) {
        setStatus("Searching Tenor needs a free API key. Get one from "
                  "Google Cloud (console.cloud.google.com), enable the Tenor API, "
                  "then paste the key here.");
    } else if (m_grid->count() == 0) {
        setStatus("Type to search. Double-click a GIF to add it to the timeline.");
    }
}

void TenorPanel::promptForApiKey() {
    bool ok = false;
    const QString key = QInputDialog::getText(
        this, "Tenor API Key",
        "Paste your Tenor API key.\n\n"
        "Create one at console.cloud.google.com, then enable the \"Tenor API\" for it.",
        QLineEdit::Normal, m_client->apiKey(), &ok);
    if (!ok) return;

    m_client->setApiKey(key.trimmed());
    QSettings().setValue("tenor/apiKey", key.trimmed());
    updateApiKeyState();
    if (m_client->hasApiKey() && !m_searchBox->text().trimmed().isEmpty()) runSearch();
}

void TenorPanel::runSearch() {
    if (!m_client->hasApiKey()) return;
    const QString query = m_searchBox->text().trimmed();
    if (query.isEmpty()) {
        m_grid->clear();
        setStatus("Type to search. Double-click a GIF to add it to the timeline.");
        return;
    }
    setStatus(QString("Searching for \u201c%1\u201d\u2026").arg(query));
    m_client->search(query);
}

void TenorPanel::showResults(const QVector<TenorGif>& results) {
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
    setStatus(QString("%1 results \u2014 double-click one to add it to the timeline.")
                  .arg(results.size()));

    for (const TenorGif& gif : results) {
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

void TenorPanel::importItem(QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();

    for (const TenorGif& gif : m_results) {
        if (gif.id != id) continue;

        // Already downloaded once — reuse it rather than fetching again.
        const QString existing = QDir(cacheDir()).absoluteFilePath(QString("tenor_%1.gif").arg(id));
        if (QFile::exists(existing)) {
            setStatus("Imported to the timeline.");
            emit gifReady(existing);
            return;
        }

        setStatus("Downloading\u2026");
        m_client->fetchBytes(QUrl(gif.fullUrl), "import:" + id);
        return;
    }
}
