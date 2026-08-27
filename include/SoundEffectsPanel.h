#pragma once

#include <QWidget>
#include <QHash>
#include <QPoint>
#include <QVector>
#include "MyInstantsClient.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QToolButton;
class QSlider;
class QTimer;
class QEventLoop;
class AudioPlayer;

// Searches MyInstants and imports the chosen sound effect onto an audio track.
//
// Deliberately the same interaction as the GIF panel: type, see a list, preview,
// then either double-click to drop it at the playhead or drag it onto a track.
// Learning one panel should teach you the other.
//
// The one structural difference is preview. A GIF previews by animating a
// thumbnail already in hand; a sound has to be heard, which means playing it.
// Previews therefore STREAM straight from the URL rather than downloading
// first — a click should make a noise immediately, not after a round trip —
// while importing and dragging download to the same on-disk cache the GIF panel
// uses, because those produce a clip that must still resolve next time the
// project is opened.
class SoundEffectsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SoundEffectsPanel(QWidget* parent = nullptr);
    ~SoundEffectsPanel() override;

signals:
    // A sound finished downloading and is on disk at `localPath`, ready to
    // import onto an audio track.
    void soundReady(const QString& localPath);

protected:
    // Watches the list for press-and-move, to turn it into a file drag. Same
    // approach and same reason as the GIF panel: at press time the payload
    // doesn't exist yet, because the mp3 hasn't been downloaded.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void runSearch();
    void showResults(const QVector<InstantSound>& results);
    void setStatus(const QString& message, bool isError = false);
    void importItem(QListWidgetItem* item);
    void previewItem(QListWidgetItem* item);
    void stopPreview();

    // Pushes the current preview level to the player and updates the readout.
    // Called both when the slider moves and just before each preview starts —
    // mpv keeps volume on the player rather than the file, but re-applying is
    // free and means a preview can never begin at the wrong level.
    void applyPreviewVolume();

    // Flips between silent and the level in use before muting.
    void togglePreviewMute();

    // Where downloaded sounds are kept. Inside the app's cache directory rather
    // than a temp dir, because a project references the file by path and it has
    // to still be there next time the project is opened.
    static QString cacheDir();

    // The cache path for `id`, or empty if it hasn't been downloaded yet.
    static QString cachedPathFor(const QString& id);

    // The details for `id` from the current results, or a default-constructed
    // sound if it isn't in them (the list was refilled by a newer search, say).
    InstantSound soundFor(const QString& id) const;

    // Returns a local path for `id`, downloading it first if needed. The
    // download runs in a nested event loop, so this CAN take a moment and the
    // caller must be prepared for the mouse button to have been released by the
    // time it returns. Empty on failure or timeout.
    QString ensureLocalSound(const QString& id);

    void startDragFor(const QString& id);

    MyInstantsClient* m_client = nullptr;
    QLineEdit* m_searchBox = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_statusLabel = nullptr;
    QToolButton* m_settingsButton = nullptr;
    QTimer* m_debounce = nullptr;

    // --- Preview level ----------------------------------------------------
    // Monitoring only. This deliberately does NOT affect the gain of a clip
    // imported from this panel: how loud something is while auditioning it is a
    // property of your headphones, and baking that into the edit would make the
    // project sound different depending on where the slider happened to sit.
    QSlider* m_volumeSlider = nullptr;
    QToolButton* m_muteButton = nullptr;
    QLabel* m_volumeLabel = nullptr;
    int m_volumeBeforeMute = 0; // 0 means "not muted"; see togglePreviewMute

    // A single player reused for every preview, so starting one sound always
    // stops the last. Separate from the timeline's own audio players — a
    // preview must not be affected by track mutes or the transport state.
    AudioPlayer* m_preview = nullptr;
    QString m_previewingId;

    // Sound effects are mastered hot — a meme clip is usually normalised to
    // peak, so a library of them at full volume is genuinely startling next to
    // whatever else is playing. Starting below full is the right default here
    // even though 100 would be right for a media player.
    static constexpr int kDefaultPreviewVolume = 55;

    QVector<InstantSound> m_results;
    QString m_pendingImportId;

    // --- Drag-out state ---------------------------------------------------
    QPoint m_pressPos;
    QString m_pressId;
    bool m_dragBusy = false;

    QEventLoop* m_dragWaitLoop = nullptr;
    QString m_dragWaitId;
    QString m_dragWaitPath;
};
