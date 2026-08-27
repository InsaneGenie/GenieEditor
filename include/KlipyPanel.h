#pragma once

#include <QWidget>
#include <QHash>
#include <QPoint>
#include <QVector>
#include "KlipyClient.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QTimer;
class QMovie;
class QBuffer;
class QEventLoop;

// Searches Klipy and imports the chosen GIF into the project.
//
// The interaction is the one people already know from chat apps: type, see a
// grid, pick one. The difference is what "pick" does — instead of sending it,
// the GIF is downloaded to a local cache folder and handed to the timeline as
// an ordinary media file.
class KlipyPanel : public QWidget {
    Q_OBJECT
public:
    explicit KlipyPanel(QWidget* parent = nullptr);
    ~KlipyPanel() override;

signals:
    // Whether this panel currently has a usable API key. MainWindow keeps the
    // GIFs dock hidden until it does -- an always-visible tab that can only ever
    // say "set an API key" reads as a broken feature to everyone who has no
    // intention of getting one.
    void apiKeyStateChanged(bool hasKey);

    // A GIF finished downloading and is on disk at `localPath`, ready to import.
    void gifReady(const QString& localPath);

protected:
    // Watches the grid's viewport to turn a press-and-move into a file drag.
    // Done here rather than by enabling QListWidget's own drag support because
    // the payload doesn't exist yet at press time — the GIF still has to be
    // downloaded before there's a local file to hand over.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void runSearch();
    void showResults(const QVector<KlipyGif>& results);
    void importItem(QListWidgetItem* item);
    void promptForApiKey();
    void updateApiKeyState();
    void setStatus(const QString& text, bool isError = false);

    // Where downloaded GIFs are kept. Inside the app's cache directory rather
    // than a temp dir, because a project references the file by path and it has
    // to still be there next time the project is opened.
    static QString cacheDir();

    // --- Drag-out ---------------------------------------------------------
    // Dragging a GIF onto the timeline lands it exactly where it's dropped,
    // which double-clicking (append to the end) can't do. The timeline already
    // accepts text/uri-list drops from the media browser, so a GIF just needs
    // to become a local file with a URL — no new drop path in Timeline.

    // The cache path for `id`, or empty if it hasn't been downloaded yet.
    static QString cachedPathFor(const QString& id);

    // The full-size URL for `id` from the current results, or empty if `id`
    // isn't in them (the grid was refilled by a newer search, say).
    QString fullUrlFor(const QString& id) const;

    // Returns a local path for `id`, downloading it first if needed. The
    // download runs in a nested event loop, so this CAN take a moment and the
    // caller must be prepared for the mouse button to have been released by
    // the time it returns. Empty on failure or timeout.
    QString ensureLocalGif(const QString& id);

    // Builds and executes the actual QDrag once a local file exists.
    void startDragFor(const QString& id);

    KlipyClient* m_client = nullptr;
    QLineEdit* m_searchBox = nullptr;
    QListWidget* m_grid = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_apiKeyButton = nullptr;
    QTimer* m_debounce = nullptr;

    QVector<KlipyGif> m_results;

    // Animated previews are held per item and only played while hovered — a
    // grid of thirty simultaneously animating GIFs is a real CPU load for no
    // benefit, since you can only look at one at a time. This mirrors what chat
    // clients do for exactly the same reason.
    struct PreviewMovie {
        QMovie* movie = nullptr;
        QBuffer* buffer = nullptr;
    };
    QHash<QString, PreviewMovie> m_previews; // keyed by Klipy id
    QString m_hoveredId;

    QString m_pendingImportId;

    // --- Drag-out state ---------------------------------------------------
    QPoint m_pressPos;              // where the left button went down in the grid
    QString m_pressId;              // the item under it; cleared once a drag starts
    bool m_dragBusy = false;        // inside startDragFor — blocks re-entry from the nested loop

    // Set by the "drag:" branch of bytesFetched while ensureLocalGif waits.
    QEventLoop* m_dragWaitLoop = nullptr;
    QString m_dragWaitId;
    QString m_dragWaitPath;
};
