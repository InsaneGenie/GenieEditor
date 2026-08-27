#pragma once

#include <QWidget>
#include <QHash>
#include <QImage>
#include <mpv/client.h>

class QPushButton;
class QStackedLayout;
class QSlider;
class QLabel;

// Embeds libmpv for preview playback, with its own transport controls
// (rewind / play-pause / fast-forward) built in as a control bar beneath
// the video, so the whole preview panel is self-contained.
//
// Uses the simple "wid" embedding approach (hand mpv a native window
// handle) rather than the render API — less code to get started with, at
// the cost of some flexibility around custom OpenGL overlays later. Swap
// to mpv_render_context_* if you need to draw overlays directly onto the
// video frame.
//
// IMPORTANT: mpv is only ever given the handle of the dedicated m_videoSurface
// child widget, never PlayerWidget's own handle — "wid" embedding means mpv
// takes over ALL rendering of that native window, so any sibling Qt widgets
// (the transport buttons) need to live in a separate window, not layered on
// top of the same one mpv is drawing into.
//
// The blackout overlay (see setBlackout) is a SIBLING of m_videoSurface —
// both live as pages in a QStackedLayout — rather than a child layered on
// top of it. mpv's native rendering into m_videoSurface bypasses Qt's usual
// child-widget compositing, so a Qt widget parented on top of it would just
// get silently painted over every frame; swapping which sibling is shown
// via the stack is what actually hides the video window.
class PlayerWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlayerWidget(QWidget* parent = nullptr);
    ~PlayerWidget() override;

    void loadFile(const QString& path);
    void seek(double seconds, bool exact = true);
    void skip(double deltaSeconds); // relative seek, e.g. -10.0 to rewind 10s
    void play();
    void pause();
    void togglePlayPause();
    bool isPaused() const;
    double positionSec() const;
    double durationSec() const;

    // Shows a real black screen (true) or the video surface (false) — see
    // the class comment for why this swaps sibling widgets rather than
    // overlaying a child on the native video surface.
    void setBlackout(bool on);

    // Composites a still image on top of the decoded video via mpv's
    // native overlay-add command — this is NOT Qt-side drawing, it's mpv
    // itself blending the image into its rendered output, which is why it
    // works even though mpv owns the native window entirely (see the class
    // comment on why a Qt-side overlay widget wouldn't work here).
    //
    // `id` distinguishes independent overlays (e.g. one per Overlay track)
    // so they can be added/updated/removed without affecting each other —
    // mpv supports multiple simultaneous overlay ids. The image buffer is
    // kept alive internally for as long as the overlay is active, since
    // mpv reads it by raw pointer (safe here because mpv is embedded
    // in-process, not a separate process).
    //
    // NOTE: this pointer-passing mechanism is one of the more unusual parts
    // of mpv's API — if overlays don't appear, this is the first thing to
    // suspect (byte order/format mismatch, or the buffer being freed too
    // early) rather than assuming the whole approach is wrong.
    void setOverlay(int id, const QImage& image, int x = 0, int y = 0);

    // The pixel size overlay x/y coordinates are measured against — mpv's
    // overlay-add places images in the video output's own coordinate space,
    // which is this surface. Normalised overlay positions get multiplied by
    // this to become concrete pixels.
    QSize overlayCanvasSize() const;

    // The native child widget mpv renders into. Exposed so OverlayStageWidget
    // can track its screen geometry — see that class's comment for why the
    // manipulation handles have to live in a separate window rather than as a
    // child of this one.
    QWidget* videoSurface() const { return m_videoSurface; }
    void clearOverlay(int id);

signals:
    void positionChanged(double seconds);
    void fileLoaded(double durationSeconds);
    void pausedChanged(bool paused);
    // Fired only when the user actually clicks the Play/Pause button —
    // distinct from pausedChanged, which also fires when MainWindow
    // programmatically pauses this instance for gap-management reasons
    // (video paused during a timeline gap while overall playback should
    // still be considered "playing" for audio-track purposes).
    void userToggledPlayback(bool nowPlaying);
    // Fired when the user clicks Rewind/Fast-forward, instead of this
    // widget adjusting its own mpv position directly. MainWindow's master
    // timeline clock is the authority on playback position (see
    // MainWindow's architecture note); a local skip() here would just get
    // silently overwritten by the very next drift-correction sync tick.
    void userRequestedSkip(double deltaSeconds);
    // Fired when the user clicks the Start/End jump buttons — same
    // reasoning as userRequestedSkip: MainWindow owns the actual timeline
    // position and knows the project's real duration (this widget doesn't),
    // so it decides what "the end" actually means and calls seekTimeline.
    void userRequestedGoToStart();
    void userRequestedGoToEnd();
    // Fired when the volume slider moves — MainWindow applies this to
    // every audio track's AudioPlayer, since real audio output comes
    // exclusively from those (this widget's own mpv instance has its
    // audio disabled entirely via aid=no; see the class comment).
    void volumeChanged(int percent);

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void command(const QVector<QString>& args);
    void handleMpvEvents();

    mpv_handle* m_mpv = nullptr;
    int m_pollTimerId = 0;

    QWidget* m_videoSurface = nullptr;    // dedicated native child window mpv renders into
    QWidget* m_blackOverlay = nullptr;    // plain black sibling widget, shown during gaps
    QStackedLayout* m_videoStack = nullptr; // swaps between the two above
    QPushButton* m_rewindButton = nullptr;
    QPushButton* m_playPauseButton = nullptr;
    QPushButton* m_fastForwardButton = nullptr;
    QPushButton* m_goToStartButton = nullptr;
    QPushButton* m_goToEndButton = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QLabel* m_volumeIcon = nullptr; // swaps to a muted glyph when the slider hits zero

    // Keeps each active overlay's pixel buffer alive — mpv's overlay-add
    // reads this memory by raw pointer, so it must not be freed/reallocated
    // while the overlay is showing. Format_ARGB32_Premultiplied is used because
    // its in-memory byte order on little-endian machines matches mpv's "bgra",
    // AND because that format is documented as premultiplied — see setOverlay.
    QHash<int, QImage> m_overlayImages;

    // The button's own notion of "should be playing" — deliberately NOT
    // derived from isPaused() (mpv's actual property). mpv gets forcibly
    // paused during timeline gaps regardless of overall play/pause intent
    // (see MainWindow::syncVideoToTimeline), so if the button asked mpv
    // "are you paused?" to decide what to do next, clicking Pause during a
    // gap would see "yes, already paused" and toggle to Play instead —
    // exactly backwards. This flag is the source of truth for the button
    // and for what gets reported via userToggledPlayback; only the button
    // click itself ever changes it.
    bool m_uiPlayingState = false;

    static constexpr double kSkipSeconds = 10.0;
};
