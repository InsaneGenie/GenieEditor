#pragma once

#include <QWidget>
#include <QHash>
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
    // A GIF finished downloading and is on disk at `localPath`, ready to import.
    void gifReady(const QString& localPath);

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
};
