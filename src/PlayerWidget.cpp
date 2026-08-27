#include "PlayerWidget.h"
#include "Theme.h"

#include <QVector>
#include <QByteArray>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <vector>
#include <cstdlib>
#include <stdexcept>

static void checkMpvError(int status) {
    if (status < 0) {
        throw std::runtime_error(mpv_error_string(status));
    }
}

PlayerWidget::PlayerWidget(QWidget* parent) : QWidget(parent) {
    // --- Video surface -----------------------------------------------
    // mpv's "wid" embedding takes over ALL rendering of the window handle
    // it's given, so it gets a dedicated child widget rather than `this` —
    // that keeps the transport buttons below (siblings, not children of
    // the video surface) safely outside mpv's rendering.
    m_videoSurface = new QWidget();
    m_videoSurface->setAttribute(Qt::WA_NativeWindow);
    m_videoSurface->setAttribute(Qt::WA_DontCreateNativeAncestors);

    // --- Blackout "page" -------------------------------------------------
    // A plain black widget, SIBLING to m_videoSurface (not its child) — see
    // the class comment in PlayerWidget.h for why this has to be a sibling
    // swapped via QStackedLayout rather than a child layered on top: mpv's
    // native rendering into m_videoSurface bypasses normal Qt compositing,
    // so anything parented on top of it just gets painted over every frame.
    m_blackOverlay = new QWidget();
    m_blackOverlay->setAutoFillBackground(true);
    m_blackOverlay->setStyleSheet("background-color: black;");

    m_videoStack = new QStackedLayout();
    m_videoStack->setContentsMargins(0, 0, 0, 0);
    m_videoStack->addWidget(m_videoSurface); // index 0 — shown by default
    m_videoStack->addWidget(m_blackOverlay); // index 1 — shown during gaps

    auto* videoContainer = new QWidget(this);
    videoContainer->setLayout(m_videoStack);

    // --- Transport controls --------------------------------------------
    // Icon buttons rather than emoji-labelled text buttons: consistent across
    // platforms, and it lets Play/Pause be a single control that swaps its glyph
    // instead of a button whose width jumps as its label changes.
    auto makeTransportButton = [this](Theme::Icon iconId, const QString& tooltip, int size) {
        auto* button = new QPushButton(this);
        button->setIcon(Theme::icon(iconId, Theme::text(), size));
        button->setIconSize(QSize(size, size));
        button->setToolTip(tooltip);
        button->setFixedSize(size + 16, size + 12);
        button->setProperty("variant", "ghost");
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    m_goToStartButton   = makeTransportButton(Theme::Icon::GoToStart, "Jump to start", 17);
    m_rewindButton      = makeTransportButton(Theme::Icon::SkipBack, "Back 10 seconds", 17);
    m_playPauseButton   = makeTransportButton(Theme::Icon::Play, "Play  (Space)", 20);
    m_fastForwardButton = makeTransportButton(Theme::Icon::SkipForward, "Forward 10 seconds", 17);
    m_goToEndButton     = makeTransportButton(Theme::Icon::GoToEnd, "Jump to end", 17);

    // Play/pause is the primary action of the whole panel, so it gets the one
    // filled, accented control in the bar — everything else stays ghosted.
    m_playPauseButton->setProperty("variant", "primary");
    m_playPauseButton->setFixedSize(44, 34);
    // Dark glyph, because the primary variant fills with the accent colour.
    m_playPauseButton->setIcon(Theme::icon(Theme::Icon::Play, QColor(0x06, 0x14, 0x17), 20));

    m_volumeIcon = new QLabel(this);
    m_volumeIcon->setPixmap(Theme::icon(Theme::Icon::Volume, Theme::textDim(), 15).pixmap(15, 15));

    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(96);
    m_volumeSlider->setToolTip("Master volume");
    m_volumeSlider->setCursor(Qt::PointingHandCursor);

    auto* controlsLayout = new QHBoxLayout();
    controlsLayout->setContentsMargins(12, 6, 12, 6);
    controlsLayout->setSpacing(6);
    controlsLayout->addStretch();
    controlsLayout->addWidget(m_goToStartButton);
    controlsLayout->addWidget(m_rewindButton);
    controlsLayout->addSpacing(4);
    controlsLayout->addWidget(m_playPauseButton);
    controlsLayout->addSpacing(4);
    controlsLayout->addWidget(m_fastForwardButton);
    controlsLayout->addWidget(m_goToEndButton);
    controlsLayout->addStretch();
    controlsLayout->addWidget(m_volumeIcon);
    controlsLayout->addWidget(m_volumeSlider);

    // The control bar gets its own surface so it reads as a distinct strip
    // beneath the video rather than floating on the same black as the frame.
    auto* controlBar = new QWidget(this);
    controlBar->setLayout(controlsLayout);
    controlBar->setAutoFillBackground(true);
    // Scoped by object name on purpose. An unscoped rule here would propagate to
    // every child widget and override their app-level styling — which is exactly
    // what silently flattened the primary Play button back to a plain outline.
    controlBar->setObjectName("PlayerControlBar");
    controlBar->setStyleSheet(QString("QWidget#PlayerControlBar { background-color: %1; border-top: 1px solid %2; }")
                                  .arg(Theme::bg2().name(), Theme::lineSoft().name()));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(videoContainer, /*stretch=*/1); // video fills all remaining space
    rootLayout->addWidget(controlBar);

    connect(m_goToStartButton, &QPushButton::clicked, this, [this] { emit userRequestedGoToStart(); });
    connect(m_rewindButton, &QPushButton::clicked, this, [this] { emit userRequestedSkip(-kSkipSeconds); });
    connect(m_fastForwardButton, &QPushButton::clicked, this, [this] { emit userRequestedSkip(kSkipSeconds); });
    connect(m_goToEndButton, &QPushButton::clicked, this, [this] { emit userRequestedGoToEnd(); });
    connect(m_volumeSlider, &QSlider::valueChanged, this, &PlayerWidget::volumeChanged);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_volumeIcon->setPixmap(Theme::icon(value == 0 ? Theme::Icon::VolumeMute : Theme::Icon::Volume,
                                            value == 0 ? Theme::textFaint() : Theme::textDim(), 15)
                                    .pixmap(15, 15));
    });
    connect(m_playPauseButton, &QPushButton::clicked, this, [this] {
        // Toggles OUR OWN tracked intent, not mpv's actual pause state —
        // see the m_uiPlayingState comment in the header for why.
        m_uiPlayingState = !m_uiPlayingState;
        m_playPauseButton->setIcon(Theme::icon(m_uiPlayingState ? Theme::Icon::Pause : Theme::Icon::Play,
                                               QColor(0x06, 0x14, 0x17), 20));
        m_playPauseButton->setToolTip(m_uiPlayingState ? "Pause  (Space)" : "Play  (Space)");
        emit userToggledPlayback(m_uiPlayingState);
    });

    // --- mpv setup -------------------------------------------------------
    m_mpv = mpv_create();
    if (!m_mpv) {
        throw std::runtime_error("failed to create mpv instance");
    }

    // Hand mpv the video surface's native window id — NOT this widget's —
    // so it renders only into that child area, leaving the buttons alone.
    int64_t wid = static_cast<int64_t>(m_videoSurface->winId());
    mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);

    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "hr-seek", "yes"); // frame-accurate seeking
    // Audio now comes exclusively from dedicated per-audio-track AudioPlayer
    // instances (see AudioPlayer.h) rather than this instance's own embedded
    // audio — otherwise you'd hear the video file's audio doubled up with
    // (or worse, drifting against) the actual Audio-track playback.
    mpv_set_option_string(m_mpv, "aid", "no");

    checkMpvError(mpv_initialize(m_mpv));

    // Poll mpv's event queue on a timer rather than wiring up its
    // wakeup-callback + cross-thread posting for this skeleton.
    m_pollTimerId = startTimer(16);
}

PlayerWidget::~PlayerWidget() {
    if (m_pollTimerId) killTimer(m_pollTimerId);
    if (m_mpv) mpv_terminate_destroy(m_mpv);
}

void PlayerWidget::command(const QVector<QString>& args) {
    std::vector<QByteArray> storage;
    std::vector<const char*> cargs;
    storage.reserve(args.size());
    cargs.reserve(args.size() + 1);
    for (const auto& a : args) {
        storage.push_back(a.toUtf8());
        cargs.push_back(storage.back().constData());
    }
    cargs.push_back(nullptr);
    mpv_command(m_mpv, cargs.data());
}

void PlayerWidget::loadFile(const QString& path) {
    command({"loadfile", path});
}

void PlayerWidget::seek(double seconds, bool exact) {
    command({"seek", QString::number(seconds, 'f', 3), "absolute",
              exact ? "exact" : "keyframes"});
}

void PlayerWidget::skip(double deltaSeconds) {
    // "relative" seeks are clamped to [0, duration] by mpv itself, so
    // rewinding past 0 or fast-forwarding past the end just clamps rather
    // than erroring.
    command({"seek", QString::number(deltaSeconds, 'f', 3), "relative"});
}

void PlayerWidget::play() {
    const int flag = 0; // pause = no
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&flag));
    emit pausedChanged(false);
}

void PlayerWidget::pause() {
    const int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, const_cast<int*>(&flag));
    emit pausedChanged(true);
}

void PlayerWidget::togglePlayPause() {
    if (isPaused()) {
        play();
    } else {
        pause();
    }
}

bool PlayerWidget::isPaused() const {
    int flag = 0;
    mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
    return flag != 0;
}

double PlayerWidget::positionSec() const {
    double pos = 0.0;
    mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double PlayerWidget::durationSec() const {
    double dur = 0.0;
    mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

void PlayerWidget::setSpeed(double speed) {
    if (!m_mpv) return;
    double rate = std::clamp(speed, Clip::kMinSpeed, Clip::kMaxSpeed);
    // Setting this every sync tick would be wasteful and can make mpv stutter,
    // so it only goes through when it actually changes.
    if (std::fabs(rate - m_speed) < 1e-6) return;
    m_speed = rate;
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &rate);
}

void PlayerWidget::setBlackout(bool on) {
    m_videoStack->setCurrentWidget(on ? static_cast<QWidget*>(m_blackOverlay)
                                       : static_cast<QWidget*>(m_videoSurface));
}

void PlayerWidget::setOverlay(int id, const QImage& image, int x, int y) {
    if (image.isNull()) {
        clearOverlay(id);
        return;
    }

    // Format_ARGB32_Premultiplied stores each pixel as 0xAARRGGBB in a 32-bit
    // word — on a little-endian CPU that's B,G,R,A in memory byte order,
    // matching mpv's "bgra" overlay format. Storing the converted image in
    // m_overlayImages (rather than a local temporary) is what keeps the buffer
    // alive for as long as mpv might read from it.
    //
    // PREMULTIPLIED is required, not a preference. mpv's overlay-add documents
    // bgra as premultiplied alpha: every colour component must already be
    // multiplied by the alpha component. Handing it straight alpha instead
    // composites as src + dst*(1-a) with full-brightness colour, so reducing
    // opacity ADDS light — a half-transparent overlay came out brighter than an
    // opaque one, which is the opposite of what the control says it does.
    QImage& stored = m_overlayImages[id];
    stored = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    const QString ptrArg = QString("&%1").arg(reinterpret_cast<quintptr>(stored.constBits()));
    command({"overlay-add", QString::number(id),
              QString::number(x), QString::number(y),
              ptrArg, "0", "bgra",
              QString::number(stored.width()),
              QString::number(stored.height()),
              QString::number(stored.bytesPerLine())});
}

QSize PlayerWidget::overlayCanvasSize() const {
    const QSize size = m_videoSurface->size();
    // Guard against the zero/degenerate size the surface reports before the
    // first layout pass — a divide or scale against that would produce a null
    // image and silently drop the overlay.
    if (size.width() < 16 || size.height() < 16) return QSize(1280, 720);
    return size;
}

void PlayerWidget::clearOverlay(int id) {
    if (!m_overlayImages.contains(id)) return;
    command({"overlay-remove", QString::number(id)});
    m_overlayImages.remove(id);
}

void PlayerWidget::timerEvent(QTimerEvent*) {
    handleMpvEvents();
    emit positionChanged(positionSec());
}

void PlayerWidget::handleMpvEvents() {
    while (true) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;

        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            emit fileLoaded(durationSec());
            // NOTE: no longer used to drive the Play/Pause button (that's
            // m_uiPlayingState now) — kept as a general status signal for
            // anything else that cares about mpv's actual pause state.
            emit pausedChanged(isPaused());
        }
    }
}
