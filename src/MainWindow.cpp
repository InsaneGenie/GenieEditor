#include "MainWindow.h"
#include "PlayerWidget.h"
#include "AudioPlayer.h"
#include "Timeline.h"
#include "TrackHeaderPanel.h"
#include "MediaBrowserPanel.h"
#include "OverlayInspectorPanel.h"
#include "OverlayStageWidget.h"
#include "KlipyPanel.h"
#include "Transcriber.h"
#include "FFmpegExporter.h"
#include "MediaProbe.h"
#include "WaveformGenerator.h"
#include "ThumbnailGenerator.h"
#include "OverlayImageLoader.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QListWidget>
#include <QTabWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QShortcut>
#include <QBrush>
#include <QColor>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QDockWidget>
#include <QStatusBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolBar>
#include <QToolButton>
#include <QFrame>
#include <QAction>
#include <QSizePolicy>
#include <QKeySequence>
#include <QSettings>
#include <QCoreApplication>
#include <QApplication>
#include <QCloseEvent>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QHash>
#include <QPair>
#include <QPainter>
#include <QSize>
#include <QProgressDialog>
#include <QDir>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Seed the project with one video track and two audio tracks —
    // matches the "multi track audio" requirement out of the gate.
    m_project.addTrack(TrackType::Video, "Video 1");
    m_project.addTrack(TrackType::Audio, "Audio 1");
    m_project.addTrack(TrackType::Audio, "Audio 2");
    m_project.tracks[0].pairedAudioTrackIndex = 1; // Video 1 <-> Audio 1

    // Each audio track gets its own headless AudioPlayer so it can play
    // back completely independently of the video track's gaps/cuts.
    for (int i = 0; i < m_project.tracks.size(); ++i) {
        if (m_project.tracks[i].type == TrackType::Audio) {
            setupAudioPlayerForTrack(i);
        }
    }

    m_masterClockTimer = new QTimer(this);
    m_masterClockTimer->setInterval(16); // matches PlayerWidget's own mpv-poll rate
    connect(m_masterClockTimer, &QTimer::timeout, this, &MainWindow::onMasterClockTick);

    buildUi();
    buildMenus();

    resize(1280, 800);
    setWindowTitle("Video Editor");

    restoreLayout();
}

void MainWindow::buildUi() {
    // Dock nesting lets panels be dragged into the same area, split,
    // tabbed, or floated as separate windows — this is what makes panels
    // truly repositionable, not just resizable.
    setDockNestingEnabled(true);

    // Corner ownership controls which dock area "wins" the four corners of
    // the window, which in turn determines whether top/bottom docks span
    // the full width or get squeezed between left/right docks. Giving top
    // corners to the top area and bottom corners to the bottom area (rather
    // than Qt's default of giving all corners to left/right) means the
    // Timeline dock spans full width along the bottom, beneath both the
    // Preview and Transcript docks — the layout most editors use.
    setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    // No fixed central widget — everything (Preview, Timeline, Transcript)
    // lives in a dock, so every panel can be dragged to any edge, tabbed
    // together, or popped out into its own floating window.

    // --- Timeline dock ---------------------------------------------------
    // Two side-by-side widgets: a fixed TrackHeaderPanel (never scrolls
    // horizontally) and the scrollable Timeline (ruler + clips). This is
    // Premiere Pro's frozen-track-header-column pattern. Their vertical
    // scrollbars are kept in sync below so both move together if there are
    // ever more tracks than fit on screen.
    m_timeline = new Timeline();
    m_timeline->setProject(&m_project);

    m_timelineScrollArea = new QScrollArea();
    m_timelineScrollArea->setWidget(m_timeline);
    // true = stretch the timeline to fill the viewport when content is
    // shorter than the window (so the track background/ruler always spans
    // the full width); once content exceeds the viewport, the timeline
    // keeps its real sizeHint() width and scrollbars kick in normally.
    m_timelineScrollArea->setWidgetResizable(true);
    m_timelineScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_timelineScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_trackHeaderPanel = new TrackHeaderPanel();
    m_trackHeaderPanel->setProject(&m_project);

    m_headerScrollArea = new QScrollArea();
    m_headerScrollArea->setWidget(m_trackHeaderPanel);
    m_headerScrollArea->setWidgetResizable(false);
    m_headerScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Vertical scrollbar stays hidden — this panel is driven programmatically
    // by Timeline's own vertical scrollbar (connected below) rather than
    // scrolled directly by the user, so both stay perfectly in sync.
    m_headerScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_timelineScrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            m_headerScrollArea->verticalScrollBar(), &QScrollBar::setValue);

    // --- Timeline tool strip ---------------------------------------------
    // Split lives here rather than in the window's main toolbar: it acts on the
    // timeline selection at the timeline playhead, so putting it inside the
    // timeline panel keeps the control next to the thing it operates on instead
    // of at the opposite end of the window. The top toolbar is left for
    // project-level actions (import, transcribe, export).
    //
    // The QAction is still owned by MainWindow and registered on the window
    // below, so Ctrl+K keeps working from anywhere — including when the
    // Timeline dock is floated into its own window or closed entirely.
    m_splitAction = new QAction(Theme::icon(Theme::Icon::Split, Theme::textDim()), "Split", this);
    m_splitAction->setToolTip("Split the selected clips at the playhead  (Ctrl+K)");
    m_splitAction->setShortcut(QKeySequence("Ctrl+K"));
    m_splitAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_splitAction, &QAction::triggered, this, &MainWindow::onSplitClicked);
    addAction(m_splitAction); // window-scoped, so the shortcut fires wherever focus is

    auto* splitButton = new QToolButton();
    splitButton->setDefaultAction(m_splitAction);
    splitButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    splitButton->setIconSize(QSize(17, 17));
    splitButton->setCursor(Qt::PointingHandCursor);

    // Pin: drops a marker at the playhead, or clears the one already there.
    // Lives beside Split because both are playhead-relative timeline edits.
    m_pinAction = new QAction(Theme::icon(Theme::Icon::Pin, Theme::accent()), "Pin", this);
    m_pinAction->setToolTip("Drop a pin at the playhead, or remove the pin already there  (M)\n"
                            "Double-click a pin in the ruler to delete it");
    m_pinAction->setShortcut(QKeySequence(Qt::Key_M));
    m_pinAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_pinAction, &QAction::triggered, this, [this] {
        m_timeline->togglePinAtPlayhead();
    });
    addAction(m_pinAction);

    auto* pinButton = new QToolButton();
    pinButton->setDefaultAction(m_pinAction);
    pinButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pinButton->setIconSize(QSize(17, 17));
    pinButton->setCursor(Qt::PointingHandCursor);

    auto* timelineToolStrip = new QWidget();
    timelineToolStrip->setObjectName("TimelineToolStrip");
    // Scoped by object name deliberately — an unscoped rule here would cascade
    // onto the QToolButton and override its app-level hover/pressed styling.
    timelineToolStrip->setStyleSheet(
        QString("QWidget#TimelineToolStrip { background-color: %1; border-bottom: 1px solid %2; }")
            .arg(Theme::bg2().name(), Theme::lineSoft().name()));
    auto* stripLayout = new QHBoxLayout(timelineToolStrip);
    stripLayout->setContentsMargins(7, 4, 7, 4);
    stripLayout->setSpacing(4);
    stripLayout->addWidget(splitButton);
    stripLayout->addWidget(pinButton);

    auto stripSeparator = []() {
        auto* line = new QFrame();
        line->setFrameShape(QFrame::VLine);
        line->setFixedWidth(1);
        line->setStyleSheet(QString("background-color: %1; border: none;").arg(Theme::line().name()));
        return line;
    };
    stripLayout->addSpacing(4);
    stripLayout->addWidget(stripSeparator());
    stripLayout->addSpacing(4);

    // Add-track and zoom both live here rather than in the window toolbar now.
    // Both act on the timeline and nothing else, so putting them in the timeline
    // panel keeps each control next to what it affects — and it leaves the top
    // toolbar as a short, unambiguous list of project-level actions.
    auto addTrackButton = [this](Theme::Icon iconId, const QString& label, const QString& tip,
                                  const QColor& tint, void (MainWindow::*slot)()) {
        auto* action = new QAction(Theme::icon(iconId, tint), label, this);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, slot);
        auto* button = new QToolButton();
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(17, 17));
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    stripLayout->addWidget(addTrackButton(Theme::Icon::AddVideo, "Video", "Add a video track",
                                          Theme::videoBase().lighter(125), &MainWindow::onAddVideoTrackClicked));
    stripLayout->addWidget(addTrackButton(Theme::Icon::AddAudio, "Audio", "Add an audio track",
                                          Theme::audioBase().lighter(125), &MainWindow::onAddAudioTrackClicked));
    stripLayout->addWidget(addTrackButton(Theme::Icon::AddOverlay, "Overlay",
                                          "Add an overlay track for stills and titles",
                                          Theme::overlayBase().lighter(125), &MainWindow::onAddOverlayTrackClicked));

    stripLayout->addStretch();

    // Zoom sits at the far right of the strip, directly above the ruler it
    // scales — icon-only, since three related controls in a row read fine
    // without labels and the strip stays compact.
    auto zoomButton = [this](Theme::Icon iconId, const QString& tip,
                              const QKeySequence& shortcut, void (MainWindow::*slot)()) {
        auto* action = new QAction(Theme::icon(iconId, Theme::textDim()), tip, this);
        action->setToolTip(shortcut.isEmpty()
            ? tip : QString("%1  (%2)").arg(tip, shortcut.toString(QKeySequence::NativeText)));
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
            action->setShortcutContext(Qt::ApplicationShortcut);
            addAction(action);
        }
        connect(action, &QAction::triggered, this, slot);
        auto* button = new QToolButton();
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(17, 17));
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    stripLayout->addWidget(zoomButton(Theme::Icon::ZoomOut, "Zoom out",
                                      QKeySequence::ZoomOut, &MainWindow::onZoomOutClicked));
    stripLayout->addWidget(zoomButton(Theme::Icon::ZoomIn, "Zoom in",
                                      QKeySequence::ZoomIn, &MainWindow::onZoomInClicked));
    stripLayout->addWidget(zoomButton(Theme::Icon::ZoomFit, "Fit the whole project to the window",
                                      QKeySequence("Shift+Z"), &MainWindow::onZoomToFitClicked));

    auto* timelineBody = new QWidget();
    auto* timelineBodyLayout = new QHBoxLayout(timelineBody);
    timelineBodyLayout->setContentsMargins(0, 0, 0, 0);
    timelineBodyLayout->setSpacing(0);
    timelineBodyLayout->addWidget(m_headerScrollArea);
    timelineBodyLayout->addWidget(m_timelineScrollArea, /*stretch=*/1);

    auto* timelineContainer = new QWidget();
    auto* timelineContainerLayout = new QVBoxLayout(timelineContainer);
    timelineContainerLayout->setContentsMargins(0, 0, 0, 0);
    timelineContainerLayout->setSpacing(0);
    timelineContainerLayout->addWidget(timelineToolStrip);
    timelineContainerLayout->addWidget(timelineBody, /*stretch=*/1);

    m_timelineDock = new QDockWidget("Timeline", this);
    m_timelineDock->setObjectName("TimelineDock");
    m_timelineDock->setWidget(timelineContainer);
    m_timelineDock->setFeatures(QDockWidget::DockWidgetMovable
                              | QDockWidget::DockWidgetFloatable
                              | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);

    // --- Media browser dock (left of Preview) -----------------------------
    m_mediaBrowser = new MediaBrowserPanel();
    connect(m_mediaBrowser, &MediaBrowserPanel::fileActivated, this, [this](const QString& path) {
        // Double-click behaves like the toolbar Import button — appends to
        // the end of the appropriate track, routed by extension the same
        // way drag-and-drop is (see onMediaDropped).
        static const QStringList audioExt = {"wav", "mp3", "m4a", "flac", "aac"};
        static const QStringList overlayExt = {"png", "jpg", "jpeg", "bmp"};
        const QString ext = QFileInfo(path).suffix().toLower();
        if (overlayExt.contains(ext)) {
            importOverlayFileAt(path, -1, m_project.durationSec());
        } else if (audioExt.contains(ext)) {
            importAudioOnlyFileAt(path, 1, m_project.durationSec());
        } else {
            importVideoFileAt(path, 0, m_project.durationSec());
        }
    });

    m_mediaBrowserDock = new QDockWidget("Media", this);
    m_mediaBrowserDock->setObjectName("MediaBrowserDock");
    m_mediaBrowserDock->setWidget(m_mediaBrowser);
    m_mediaBrowserDock->setFeatures(QDockWidget::DockWidgetMovable
                                  | QDockWidget::DockWidgetFloatable
                                  | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::TopDockWidgetArea, m_mediaBrowserDock);

    // --- Preview player dock -------------------------------------------
    m_player = new PlayerWidget();
    m_player->setMinimumSize(480, 270);

    m_playerDock = new QDockWidget("Preview", this);
    m_playerDock->setObjectName("PreviewDock"); // needed for saveState/restoreState
    m_playerDock->setWidget(m_player);
    m_playerDock->setFeatures(QDockWidget::DockWidgetMovable
                             | QDockWidget::DockWidgetFloatable
                             | QDockWidget::DockWidgetClosable);
    // Explicitly place Preview to the RIGHT of the media browser (rather
    // than addDockWidget, which would just stack them) — this is what
    // actually achieves "media browser to the left of preview" as the
    // default layout.
    splitDockWidget(m_mediaBrowserDock, m_playerDock, Qt::Horizontal);

    // --- Transcript dock -------------------------------------------
    // One tab per audio track (see rebuildTranscriptTabs) rather than a
    // single flat list — each track's transcript lives on the Track
    // itself now (Track::transcript), so tabs are just a view over that.
    m_transcriptTabs = new QTabWidget();

    m_transcriptSearchBox = new QLineEdit();
    m_transcriptSearchBox->setPlaceholderText("Find in transcript");
    m_transcriptSearchBox->setToolTip("Enter for the next match, Shift+Enter for the previous, Esc to clear");
    m_transcriptSearchBox->setClearButtonEnabled(true);
    // A leading magnifier inside the field, so the control announces what it is
    // without needing a separate label eating a row.
    m_transcriptSearchBox->addAction(Theme::icon(Theme::Icon::Search, Theme::textFaint(), 14),
                                     QLineEdit::LeadingPosition);
    // Visible from the start rather than hidden until Ctrl+F. It used to leave a
    // blank strip at the top of the panel and gave no hint the transcript was
    // searchable at all; Ctrl+F now just focuses it.
    connect(m_transcriptSearchBox, &QLineEdit::textChanged, this, &MainWindow::onTranscriptSearchTextChanged);
    connect(m_transcriptSearchBox, &QLineEdit::returnPressed, this, [this] {
        if (QApplication::keyboardModifiers() & Qt::ShiftModifier) onTranscriptSearchPrev();
        else onTranscriptSearchNext();
    });
    auto* transcriptSearchEscape = new QShortcut(QKeySequence(Qt::Key_Escape), m_transcriptSearchBox);
    connect(transcriptSearchEscape, &QShortcut::activated, this, [this] {
        // Clears rather than hides now that the field is permanently visible —
        // triggers textChanged("") which clears the highlights.
        m_transcriptSearchBox->clear();
        m_transcriptTabs->setFocus();
    });
    // Re-running the search when switching tabs matches how the search bar
    // already behaves after re-transcribing — searching stays scoped to
    // whichever tab is actually visible right now.
    connect(m_transcriptTabs, &QTabWidget::currentChanged, this, [this](int) {
        if (!m_transcriptSearchBox->text().isEmpty()) {
            onTranscriptSearchTextChanged(m_transcriptSearchBox->text());
        }
    });

    // The Clear Transcript button is gone along with the Transcribe button:
    // transcripts are derived from the audio now, not authored, so clearing one
    // would only mean "throw this away until the next scan puts it back".
    //
    // This replaces both, and appears only when there's no whisper model to
    // work with — which is the one case automatic transcription genuinely
    // cannot resolve on its own.
    m_chooseModelButton = new QPushButton("Choose speech model\u2026");
    m_chooseModelButton->setToolTip(
        "Transcription needs a whisper.cpp model file.\n"
        "Get one from https://huggingface.co/ggerganov/whisper.cpp/tree/main");
    m_chooseModelButton->setProperty("variant", "primary");
    m_chooseModelButton->setCursor(Qt::PointingHandCursor);
    m_chooseModelButton->hide();
    connect(m_chooseModelButton, &QPushButton::clicked, this, [this] {
        if (resolveWhisperModelPath(/*allowPrompt=*/true).isEmpty()) return;
        m_chooseModelButton->hide();
        // A model just became available, so anything that was skipped for want
        // of one is now workable.
        scheduleTranscriptionScan();
    });

    auto* transcriptTopRow = new QHBoxLayout();
    transcriptTopRow->setContentsMargins(8, 8, 8, 8);
    transcriptTopRow->setSpacing(6);
    transcriptTopRow->addWidget(m_transcriptSearchBox, /*stretch=*/1);
    transcriptTopRow->addWidget(m_chooseModelButton);

    auto* transcriptContainer = new QWidget();
    auto* transcriptLayout = new QVBoxLayout(transcriptContainer);
    transcriptLayout->setContentsMargins(0, 0, 0, 0);
    transcriptLayout->setSpacing(0);
    transcriptLayout->addLayout(transcriptTopRow);
    transcriptLayout->addWidget(m_transcriptTabs);

    // Ctrl+F anywhere in the window opens/focuses this search bar — there's
    // currently only one searchable panel, so no need to scope this to
    // whichever widget has focus.
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, &MainWindow::showTranscriptSearch);

    m_transcriptDock = new QDockWidget("Transcript", this);
    m_transcriptDock->setObjectName("TranscriptDock");
    m_transcriptDock->setWidget(transcriptContainer);
    m_transcriptDock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);
    // splitDockWidget rather than addDockWidget(RightDockWidgetArea): with no
    // central widget, the right dock area expands to claim the entire middle of
    // the window. Splitting it off the Preview instead seats it as a proper
    // third column beside the player, which is where a transcript belongs while
    // you're cutting to it.
    splitDockWidget(m_playerDock, m_transcriptDock, Qt::Horizontal);

    // --- Overlay inspector dock -------------------------------------------
    // Tabbed with the Transcript rather than given its own column: both are
    // context panels you consult while working on one specific thing, and only
    // one of them is ever relevant at a time.
    m_overlayInspector = new OverlayInspectorPanel();
    connect(m_overlayInspector, &OverlayInspectorPanel::seekRequested,
            this, &MainWindow::seekTimeline);
    connect(m_overlayInspector, &OverlayInspectorPanel::animationChanged, this, [this] {
        syncOverlaysToTimeline(m_currentTimelineSec);
        if (m_overlayStage) m_overlayStage->refresh();
        m_timeline->update();
    });

    // --- Klipy GIF dock ---------------------------------------------------
    // Tabbed with the Media browser rather than given its own column: both are
    // places you go to FIND something to put on the timeline, and you're only
    // ever using one at a time.
    m_klipyPanel = new KlipyPanel();
    connect(m_klipyPanel, &KlipyPanel::gifReady, this, [this](const QString& path) {
        // GIFs land on a VIDEO track, not an overlay track. Overlay clips are a
        // single still QImage plus animated transforms — putting an animated GIF
        // there would show one frozen frame. The video path decodes it properly
        // in both the mpv preview and the FFmpeg export.
        importVideoFileAt(path, /*videoTrackIndex=*/-1, m_project.durationSec());
    });

    m_klipyDock = new QDockWidget("GIFs", this);
    m_klipyDock->setObjectName("KlipyDock");
    m_klipyDock->setWidget(m_klipyPanel);
    m_klipyDock->setFeatures(QDockWidget::DockWidgetMovable
                           | QDockWidget::DockWidgetFloatable
                           | QDockWidget::DockWidgetClosable);
    tabifyDockWidget(m_mediaBrowserDock, m_klipyDock);
    m_mediaBrowserDock->raise();

    // Direct manipulation on the preview itself. Shares the inspector's notion of
    // which clip is selected, and both funnel their edits through
    // OverlayAnimation::applyValue, so dragging a handle and moving a slider are
    // literally the same operation.
    m_overlayStage = new OverlayStageWidget(this);
    m_overlayStage->attachTo(m_player->videoSurface());
    connect(m_overlayStage, &OverlayStageWidget::transformChanged, this, [this] {
        syncOverlaysToTimeline(m_currentTimelineSec);
        m_overlayInspector->setPlayheadSec(m_currentTimelineSec); // pull the sliders along

        // The timeline only shows keyframe diamonds, which can't move while the
        // pointer is down — repainting the whole (wide, thumbnail-heavy) widget
        // on every mouse move was pure waste. Once on release is enough.
        if (!m_overlayStage->isDragging()) m_timeline->update();
    });

    m_overlayDock = new QDockWidget("Overlay", this);
    m_overlayDock->setObjectName("OverlayDock");
    m_overlayDock->setWidget(m_overlayInspector);
    m_overlayDock->setFeatures(QDockWidget::DockWidgetMovable
                             | QDockWidget::DockWidgetFloatable
                             | QDockWidget::DockWidgetClosable);
    tabifyDockWidget(m_transcriptDock, m_overlayDock);

    buildStatusBar();

    connect(m_player, &PlayerWidget::fileLoaded, this, &MainWindow::onPlayerFileLoaded);
    connect(m_player, &PlayerWidget::userToggledPlayback, this, &MainWindow::onUserToggledPlayback);
    connect(m_player, &PlayerWidget::userRequestedSkip, this, &MainWindow::onUserRequestedSkip);
    connect(m_player, &PlayerWidget::userRequestedGoToStart, this, &MainWindow::onUserRequestedGoToStart);
    connect(m_player, &PlayerWidget::userRequestedGoToEnd, this, &MainWindow::onUserRequestedGoToEnd);
    connect(m_player, &PlayerWidget::volumeChanged, this, &MainWindow::onVolumeChanged);
    connect(m_timeline, &Timeline::seekRequested, this, &MainWindow::onTimelineSeekRequested);
    connect(m_timeline, &Timeline::clipSelected, this, &MainWindow::onClipSelected);
    connect(m_timeline, &Timeline::clipSelected, this, [this](int trackIndex, int clipIndex) {
        setOverlaySelection(trackIndex, clipIndex);
    });
    connect(m_timeline, &Timeline::mediaDropped, this, &MainWindow::onMediaDropped);
    connect(m_timeline, &Timeline::thumbnailDetailNeeded, this, &MainWindow::onThumbnailDetailNeeded);
    connect(m_timeline, &Timeline::zoomAnchorChanged, this, &MainWindow::onTimelineZoomAnchorChanged);
    connect(m_timeline, &Timeline::horizontalScrollRequested, this, [this](int deltaPixels) {
        QScrollBar* hbar = m_timelineScrollArea->horizontalScrollBar();
        hbar->setValue(hbar->value() + deltaPixels); // scrollbar clamps to valid range automatically
    });
    connect(m_timeline, &Timeline::clipDeleted, this, &MainWindow::onClipDeleted);
    connect(m_trackHeaderPanel, &TrackHeaderPanel::muteToggled, this, [this](int) {
        m_timeline->update(); // repaint so the muted track's clips dim
    });
    connect(m_trackHeaderPanel, &TrackHeaderPanel::trackEnabledChanged, this, &MainWindow::onTrackEnabledChanged);
    connect(m_trackHeaderPanel, &TrackHeaderPanel::deleteTrackRequested, this, &MainWindow::onDeleteTrackRequested);
    connect(m_trackHeaderPanel, &TrackHeaderPanel::trackVolumeChanged, this, &MainWindow::onTrackVolumeChanged);
    connect(m_trackHeaderPanel, &TrackHeaderPanel::trackRenamed, this, [this](int) {
        rebuildTranscriptTabs(); // tab label needs to match the new name
    });
    connect(m_trackHeaderPanel, &TrackHeaderPanel::trackHeightChanged, this, [this] {
        m_timeline->updateGeometry();
        m_timeline->update();
    });

    rebuildTranscriptTabs(); // seed tabs for the initial Audio 1 / Audio 2 tracks
}

void MainWindow::buildStatusBar() {
    // --- Status bar ------------------------------------------------------
    // The status bar is where transcription progress lives: always visible,
    // anchored to the bottom, and never at risk of being pushed into an
    // overflow menu.
    m_projectStatsLabel = new QLabel(this);
    m_projectStatsLabel->setProperty("variant", "caption");
    statusBar()->addWidget(m_projectStatsLabel);

    m_transcribeProgressBar = new QProgressBar();
    m_transcribeProgressBar->setRange(0, 100);
    m_transcribeProgressBar->setFixedWidth(180);
    m_transcribeProgressBar->setTextVisible(false);
    m_transcribeProgressBar->hide(); // shown only while a transcription is running

    m_transcribeStatusLabel = new QLabel("Transcribing");
    m_transcribeStatusLabel->setProperty("variant", "caption");
    m_transcribeStatusLabel->hide();

    statusBar()->addPermanentWidget(m_transcribeStatusLabel);
    statusBar()->addPermanentWidget(m_transcribeProgressBar);

    updateProjectStats();
}

void MainWindow::setOverlaySelection(int trackIndex, int clipIndex) {
    m_overlayInspector->setTarget(&m_project, trackIndex, clipIndex);
    m_overlayInspector->setPlayheadSec(m_currentTimelineSec);
    if (m_overlayStage) {
        m_overlayStage->setPlayheadSec(m_currentTimelineSec);
        m_overlayStage->setTarget(&m_project, trackIndex, clipIndex);
    }

    // Selecting an overlay clip raises its inspector: without this the panel
    // would update silently behind the Transcript tab and look broken.
    if (trackIndex >= 0 && trackIndex < m_project.tracks.size()
        && m_project.tracks[trackIndex].type == TrackType::Overlay) {
        m_overlayDock->show();
        m_overlayDock->raise();
    }
}

void MainWindow::updateProjectStats() {
    if (!m_projectStatsLabel) return;

    int clipCount = 0;
    for (const auto& track : m_project.tracks) clipCount += track.clips.size();

    const double dur = m_project.durationSec();
    const int totalSecs = static_cast<int>(dur + 0.5);
    const QString durText = QString("%1:%2:%3")
        .arg(totalSecs / 3600, 2, 10, QChar('0'))
        .arg((totalSecs % 3600) / 60, 2, 10, QChar('0'))
        .arg(totalSecs % 60, 2, 10, QChar('0'));

    m_projectStatsLabel->setText(
        QString("  %1 track%2   ·   %3 clip%4   ·   %5")
            .arg(m_project.tracks.size())
            .arg(m_project.tracks.size() == 1 ? "" : "s")
            .arg(clipCount)
            .arg(clipCount == 1 ? "" : "s")
            .arg(durText));
}

void MainWindow::applyDefaultLayout() {
    // Without a central widget, Qt hands leftover space to whichever dock area
    // will take it — which is how the Transcript panel ended up as a full-width
    // band across the middle while the Timeline got squeezed into a sliver.
    // These explicit proportions establish the layout an editor actually wants:
    // a wide preview up top with the bins either side of it, and roughly the
    // bottom two-fifths of the window given to the timeline.
    resizeDocks({m_mediaBrowserDock, m_playerDock, m_transcriptDock},
                {260, 900, 340}, Qt::Horizontal);
    resizeDocks({m_playerDock, m_timelineDock}, {560, 400}, Qt::Vertical);
}

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("Import…", this, &MainWindow::onImportClicked);
    fileMenu->addAction("Export…", this, &MainWindow::onExportClicked);
    fileMenu->addSeparator();
    fileMenu->addAction("Quit", this, &QWidget::close);

    // Standard "View" menu for toggling dock/toolbar visibility back on if
    // the user closes one — otherwise a closed dock has no way to reopen.
    auto* viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction(m_playerDock->toggleViewAction());
    viewMenu->addAction(m_transcriptDock->toggleViewAction());
    viewMenu->addAction(m_timelineDock->toggleViewAction());
    viewMenu->addAction(m_mediaBrowserDock->toggleViewAction());
    viewMenu->addAction(m_overlayDock->toggleViewAction());
    viewMenu->addAction(m_klipyDock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction("Reset panel layout", this, &MainWindow::resetLayout);
}

void MainWindow::restoreLayout() {
    QSettings settings("VideoEditorProject", "VideoEditor");
    if (settings.contains("mainWindow/geometry")) {
        restoreGeometry(settings.value("mainWindow/geometry").toByteArray());
    }

    // Layouts saved by an older build describe the OLD default arrangement, and
    // restoreState would faithfully bring it back — meaning an existing install
    // would upgrade and see none of the new layout. Bumping kLayoutVersion
    // forces the new default through exactly once, after which the user's own
    // arrangement is saved and respected as normal.
    const int savedVersion = settings.value("mainWindow/layoutVersion", 0).toInt();
    if (savedVersion == kLayoutVersion && settings.contains("mainWindow/state")) {
        // restoreState matches docks/toolbars by their objectName, which is
        // why every dock and toolbar above has one set explicitly.
        restoreState(settings.value("mainWindow/state").toByteArray());
    } else {
        applyDefaultLayout();
    }
}

void MainWindow::resetLayout() {
    // Docks can be dragged into arrangements that are hard to undo by hand —
    // and a closed dock with its View-menu entry also hidden is genuinely
    // unrecoverable. This is the way back.
    for (QDockWidget* dock : {m_mediaBrowserDock, m_klipyDock, m_playerDock,
                              m_transcriptDock, m_overlayDock, m_timelineDock}) {
        if (dock) dock->setFloating(false);
        if (dock) dock->show();
    }
    addDockWidget(Qt::TopDockWidgetArea, m_mediaBrowserDock);
    tabifyDockWidget(m_mediaBrowserDock, m_klipyDock);
    m_mediaBrowserDock->raise();
    splitDockWidget(m_mediaBrowserDock, m_playerDock, Qt::Horizontal);
    splitDockWidget(m_playerDock, m_transcriptDock, Qt::Horizontal);
    tabifyDockWidget(m_transcriptDock, m_overlayDock);
    m_transcriptDock->raise();
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    applyDefaultLayout();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings("VideoEditorProject", "VideoEditor");
    settings.setValue("mainWindow/geometry", saveGeometry());
    settings.setValue("mainWindow/state", saveState());
    settings.setValue("mainWindow/layoutVersion", kLayoutVersion);
    QMainWindow::closeEvent(event);
}

void MainWindow::onImportClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Import Media", QString(), "Media files (*.mp4 *.mov *.mkv *.wav *.mp3)");
    if (path.isEmpty()) return;

    importVideoFileAt(path, 0, m_project.durationSec());
}

void MainWindow::importVideoFileAt(const QString& path, int videoTrackIndex, double trackPosSec) {
    scheduleTranscriptionScan(); // a new source file may need transcribing
    // Fall back to the primary video track if the target isn't actually a
    // valid video track (e.g. called with a stale/out-of-range index).
    if (videoTrackIndex < 0 || videoTrackIndex >= m_project.tracks.size()
        || m_project.tracks[videoTrackIndex].type != TrackType::Video) {
        videoTrackIndex = 0;
    }

    // Route the companion audio to THIS video track's paired audio track
    // (set up when the track was created — see onAddVideoTrackClicked),
    // rather than always dumping into Audio 1 regardless of which video
    // track was actually targeted.
    int audioTrackIndex = m_project.tracks[videoTrackIndex].pairedAudioTrackIndex;
    if (audioTrackIndex < 0 || audioTrackIndex >= m_project.tracks.size()
        || m_project.tracks[audioTrackIndex].type != TrackType::Audio) {
        audioTrackIndex = 1; // Audio 1 fallback for tracks with no pairing set
    }

    double duration = MediaProbe::probeDurationSeconds(path); // cheap — header read only
    if (duration <= 0.0) {
        // Fall back to a placeholder rather than a zero-length (invisible,
        // unselectable) clip if probing fails for some reason.
        QMessageBox::warning(this, "Import",
            "Couldn't read this file's duration — using a 10s placeholder. "
            "The file may be corrupt or an unsupported format.");
        duration = 10.0;
    }

    // Clips are added immediately, WITHOUT thumbnails/waveform, so the drop
    // itself feels instant — thumbnail generation (several seeks/decodes)
    // and waveform generation (a full audio decode) are genuinely expensive
    // and were previously run synchronously right here, which is exactly
    // what made drag-and-drop feel laggy. They're filled in a moment later
    // via QtConcurrent once the background work finishes.
    Clip videoClip;
    videoClip.sourcePath = path;
    videoClip.sourceInSec = 0.0;
    videoClip.sourceOutSec = duration;
    videoClip.trackPosSec = trackPosSec;
    m_project.tracks[videoTrackIndex].clips.push_back(videoClip);
    const int videoClipIndex = m_project.tracks[videoTrackIndex].clips.size() - 1;

    Clip audioClip;
    audioClip.sourcePath = path;
    audioClip.sourceInSec = 0.0;
    audioClip.sourceOutSec = duration;
    audioClip.trackPosSec = trackPosSec;
    m_project.tracks[audioTrackIndex].clips.push_back(audioClip);
    const int audioClipIndex = m_project.tracks[audioTrackIndex].clips.size() - 1;

    m_timeline->setProject(&m_project); // clip appears immediately, just without visuals yet
    seekTimeline(trackPosSec); // preview the newly imported clip

    // Background thumbnail generation. Guards against the clip having been
    // deleted or the index no longer matching this exact file (e.g. clips
    // removed before this finishes) by re-checking both before writing back.
    auto* thumbWatcher = new QFutureWatcher<ThumbnailStrip>(this);
    connect(thumbWatcher, &QFutureWatcher<ThumbnailStrip>::finished, this,
            [this, thumbWatcher, path, videoTrackIndex, videoClipIndex] {
        const ThumbnailStrip strip = thumbWatcher->result();
        thumbWatcher->deleteLater();
        auto& clips = m_project.tracks[videoTrackIndex].clips;
        if (videoClipIndex >= 0 && videoClipIndex < clips.size() && clips[videoClipIndex].sourcePath == path) {
            Clip& c = clips[videoClipIndex];
            c.thumbnails = strip.frames;
            c.thumbnailSourceDurationSec = strip.durationSec > 0.0 ? strip.durationSec : c.durationSec();
            m_timeline->update();
        }
    });
    thumbWatcher->setFuture(QtConcurrent::run(&ThumbnailGenerator::generate, path, 12, 120, 68));

    // Background waveform generation, same pattern.
    auto* waveWatcher = new QFutureWatcher<WaveformData>(this);
    connect(waveWatcher, &QFutureWatcher<WaveformData>::finished, this,
            [this, waveWatcher, path, audioTrackIndex, audioClipIndex] {
        const WaveformData waveform = waveWatcher->result();
        waveWatcher->deleteLater();
        auto& clips = m_project.tracks[audioTrackIndex].clips;
        if (audioClipIndex >= 0 && audioClipIndex < clips.size() && clips[audioClipIndex].sourcePath == path) {
            Clip& c = clips[audioClipIndex];
            c.waveformPeaks = waveform.peaks;
            c.waveformSourceDurationSec = waveform.durationSec > 0.0 ? waveform.durationSec : c.durationSec();
            m_timeline->update();
        }
    });
    waveWatcher->setFuture(QtConcurrent::run(&WaveformGenerator::generate, path, 2000));
}

void MainWindow::importOverlayFileAt(const QString& path, int overlayTrackIndex, double trackPosSec) {
    // Fall back to the first existing Overlay track, or create one if none
    // exists yet — avoids a dead-end where dropping an overlay image before
    // ever clicking "+ Overlay Track" would otherwise silently do nothing.
    bool validTarget = overlayTrackIndex >= 0 && overlayTrackIndex < m_project.tracks.size()
                     && m_project.tracks[overlayTrackIndex].type == TrackType::Overlay;
    if (!validTarget) {
        overlayTrackIndex = -1;
        for (int i = 0; i < m_project.tracks.size(); ++i) {
            if (m_project.tracks[i].type == TrackType::Overlay) { overlayTrackIndex = i; break; }
        }
        if (overlayTrackIndex < 0) {
            m_project.addTrack(TrackType::Overlay, "Overlay 1");
            overlayTrackIndex = m_project.tracks.size() - 1;
        }
    }

    Clip overlayClip;
    overlayClip.sourcePath = path;
    overlayClip.sourceInSec = 0.0;
    overlayClip.sourceOutSec = kDefaultOverlayClipLenSec; // stills have no intrinsic duration
    overlayClip.trackPosSec = trackPosSec;
    m_project.tracks[overlayTrackIndex].clips.push_back(overlayClip);

    m_timeline->setProject(&m_project);
    m_trackHeaderPanel->setProject(&m_project);
    seekTimeline(m_currentTimelineSec); // re-evaluate immediately so a freshly-dropped overlay at the current position shows right away
}

void MainWindow::onAddOverlayTrackClicked() {
    int overlayCount = 0;
    for (const auto& t : m_project.tracks) {
        if (t.type == TrackType::Overlay) ++overlayCount;
    }
    m_project.addTrack(TrackType::Overlay, QString("Overlay %1").arg(overlayCount + 1));

    refreshTrackViews();
}

void MainWindow::onTrackEnabledChanged(int /*trackIndex*/) {
    m_timeline->update(); // repaint so the disabled track's dimming shows
    seekTimeline(m_currentTimelineSec); // re-evaluate immediately — this track's inclusion in preview just changed
}

void MainWindow::onDeleteTrackRequested(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= m_project.tracks.size()) return;

    const Track& target = m_project.tracks[trackIndex];
    if (!target.clips.isEmpty()) {
        const auto reply = QMessageBox::question(this, "Delete Track",
            QString("Delete \"%1\" and its %2 clip(s)? This can't be undone.")
                .arg(target.name).arg(target.clips.size()));
        if (reply != QMessageBox::Yes) return;
    }

    const TrackType removedType = target.type;

    // Tear down this track's own playback resources BEFORE removing it
    // from the project, while trackIndex still correctly identifies it.
    if (removedType == TrackType::Audio) {
        for (int i = 0; i < m_audioTracks.size(); ++i) {
            if (m_audioTracks[i].trackIndex == trackIndex) {
                m_audioTracks[i].player->deleteLater();
                m_audioTracks.removeAt(i);
                break;
            }
        }
    }

    // Overlay ids are DERIVED from track index (t + 1 in
    // syncOverlaysToTimeline), so removing a track shifts what id every
    // later overlay track should use. Rather than trying to remap ids in
    // place, just clear every currently-shown overlay under its OLD id and
    // let syncOverlaysToTimeline() naturally re-populate everything fresh
    // (with correct new ids) on the very next sync — much simpler than
    // in-place remapping, and just as correct.
    for (auto it = m_activeOverlayClipByTrack.constBegin(); it != m_activeOverlayClipByTrack.constEnd(); ++it) {
        if (it.value() != -1) {
            m_player->clearOverlay(it.key() + 1);
        }
    }
    m_activeOverlayClipByTrack.clear();

    m_project.removeTrack(trackIndex);

    // Re-index the one piece of per-track state Project doesn't know about
    // and that DOESN'T need the clear-and-repopulate treatment: the
    // AudioPlayer lookup table just needs its indices shifted down.
    for (auto& audio : m_audioTracks) {
        if (audio.trackIndex > trackIndex) --audio.trackIndex;
    }

    // Timeline's own selection encodes trackIndex directly — indices at or
    // above the removed track may now point at the wrong track, so clear
    // rather than risk acting on the wrong clip.
    m_timeline->clearSelection();
    setOverlaySelection(-1, -1); // the panel and handles may be pointing at a deleted clip

    refreshTrackViews();

    // Force a fresh evaluation of what should currently be playing —
    // whatever was loaded may no longer be valid after the removal.
    m_playingVideoTrackIndex = -1;
    m_playingClipIndex = -1;
    m_currentLoadedPath.clear();
    seekTimeline(m_currentTimelineSec);
}

void MainWindow::refreshTrackViews() {
    updateProjectStats();
    scheduleTranscriptionScan(); // the track or clip set just changed
    m_timeline->setProject(&m_project);
    m_trackHeaderPanel->setProject(&m_project);
    // Extra nudge at the scroll-area level itself, in addition to each
    // widget's own setProject()-driven resize — belt-and-suspenders against
    // any remaining Qt layout-timing edge case specifically around
    // scrollbar RANGE not reflecting a just-changed track count.
    m_timelineScrollArea->updateGeometry();
    m_headerScrollArea->updateGeometry();
    rebuildTranscriptTabs(); // audio track count may have changed — tabs need to match
}

void MainWindow::setupAudioPlayerForTrack(int trackIndex) {
    AudioTrackPlayback audio;
    audio.trackIndex = trackIndex;
    audio.player = new AudioPlayer(this);
    audio.player->setVolume(combinedVolumeForTrack(trackIndex)); // start at whatever level the user already set (master * this track's own, which defaults to 100)
    connect(audio.player, &AudioPlayer::fileLoaded, this, [this, trackIndex](double) {
        for (auto& a : m_audioTracks) {
            if (a.trackIndex != trackIndex) continue;
            if (a.awaitingSeekAfterLoad) {
                a.player->seek(a.pendingSeekSec);
                a.awaitingSeekAfterLoad = false;
            }
            // mpv autoplays on load by default — respect current
            // pause/play intent rather than always starting playback.
            if (!m_isPlayingIntent) a.player->pause();
            break;
        }
    });
    m_audioTracks.push_back(audio);
}

void MainWindow::importAudioOnlyFileAt(const QString& path, int trackIndex, double trackPosSec) {
    scheduleTranscriptionScan(); // a new source file may need transcribing
    // Fall back to Audio 1 if the drop landed somewhere that isn't actually
    // an audio track (e.g. dropped onto the video lane, or the ruler).
    if (trackIndex < 0 || trackIndex >= m_project.tracks.size()
        || m_project.tracks[trackIndex].type != TrackType::Audio) {
        trackIndex = 1;
    }

    double duration = MediaProbe::probeDurationSeconds(path);
    if (duration <= 0.0) {
        QMessageBox::warning(this, "Import",
            "Couldn't read this file's duration — using a 10s placeholder. "
            "The file may be corrupt or an unsupported format.");
        duration = 10.0;
    }

    Clip audioClip;
    audioClip.sourcePath = path;
    audioClip.sourceInSec = 0.0;
    audioClip.sourceOutSec = duration;
    audioClip.trackPosSec = trackPosSec;
    m_project.tracks[trackIndex].clips.push_back(audioClip);
    const int clipIndex = m_project.tracks[trackIndex].clips.size() - 1;

    m_timeline->setProject(&m_project);
    seekTimeline(trackPosSec);

    auto* waveWatcher = new QFutureWatcher<WaveformData>(this);
    connect(waveWatcher, &QFutureWatcher<WaveformData>::finished, this,
            [this, waveWatcher, path, trackIndex, clipIndex] {
        const WaveformData waveform = waveWatcher->result();
        waveWatcher->deleteLater();
        auto& clips = m_project.tracks[trackIndex].clips;
        if (clipIndex >= 0 && clipIndex < clips.size() && clips[clipIndex].sourcePath == path) {
            Clip& c = clips[clipIndex];
            c.waveformPeaks = waveform.peaks;
            c.waveformSourceDurationSec = waveform.durationSec > 0.0 ? waveform.durationSec : c.durationSec();
            m_timeline->update();
        }
    });
    waveWatcher->setFuture(QtConcurrent::run(&WaveformGenerator::generate, path, 2000));
}

void MainWindow::onMediaDropped(const QString& filePath, int trackIndex, double timelineSec) {
    static const QStringList kAudioOnlyExtensions = {"wav", "mp3", "m4a", "flac", "aac"};
    static const QStringList kOverlayExtensions = {"png", "jpg", "jpeg", "bmp"};
    const QString ext = QFileInfo(filePath).suffix().toLower();

    if (kOverlayExtensions.contains(ext)) {
        importOverlayFileAt(filePath, trackIndex, timelineSec);
    } else if (kAudioOnlyExtensions.contains(ext)) {
        importAudioOnlyFileAt(filePath, trackIndex, timelineSec);
    } else {
        // Route to whichever track was actually dropped onto, IF it's a
        // video track — this is what makes dropping onto a newly-added
        // video track actually land there instead of always defaulting to
        // Video 1 regardless of where you dropped.
        importVideoFileAt(filePath, trackIndex, timelineSec);
    }
}

void MainWindow::onThumbnailDetailNeeded(int trackIndex, int clipIndex, int desiredFullFileFrameCount) {
    if (trackIndex < 0 || trackIndex >= m_project.tracks.size()) return;
    auto& clips = m_project.tracks[trackIndex].clips;
    if (clipIndex < 0 || clipIndex >= clips.size()) return;
    Clip& clip = clips[clipIndex];

    const qint64 key = (static_cast<qint64>(trackIndex) << 32) | static_cast<quint32>(clipIndex);
    if (m_pendingThumbnailUpgrades.contains(key)) return; // already regenerating this exact clip

    // Only worth the decode cost if this is a MEANINGFUL improvement over
    // what's already cached — avoids re-triggering on every tiny zoom step.
    if (desiredFullFileFrameCount <= clip.thumbnails.size() * 1.3) return;

    m_pendingThumbnailUpgrades.insert(key);
    const QString path = clip.sourcePath;

    auto* watcher = new QFutureWatcher<ThumbnailStrip>(this);
    connect(watcher, &QFutureWatcher<ThumbnailStrip>::finished, this,
            [this, watcher, path, trackIndex, clipIndex, key] {
        const ThumbnailStrip strip = watcher->result();
        watcher->deleteLater();
        m_pendingThumbnailUpgrades.remove(key);

        if (trackIndex >= m_project.tracks.size()) return;
        auto& refreshedClips = m_project.tracks[trackIndex].clips;
        if (clipIndex < 0 || clipIndex >= refreshedClips.size()) return;
        Clip& c = refreshedClips[clipIndex];
        // Guards against the clip having been deleted/replaced, or the
        // user having zoomed back out while this was generating (in which
        // case the result may no longer actually be an improvement).
        if (c.sourcePath == path && strip.frames.size() > c.thumbnails.size()) {
            c.thumbnails = strip.frames;
            c.thumbnailSourceDurationSec = strip.durationSec > 0.0 ? strip.durationSec : c.durationSec();
            m_timeline->update();
        }
    });
    watcher->setFuture(QtConcurrent::run(&ThumbnailGenerator::generate, path, desiredFullFileFrameCount, 120, 68));
}

void MainWindow::onTimelineZoomAnchorChanged(double anchorSec, int oldPixelX) {
    // The anchor second's NEW pixel-space x-coordinate, at the zoom level
    // Timeline just applied (pixelsPerSecond() already reflects it).
    const int newPixelX = static_cast<int>(anchorSec * m_timeline->pixelsPerSecond());

    QScrollBar* hbar = m_timelineScrollArea->horizontalScrollBar();
    // Shifting the scroll position by exactly how much the anchor's pixel
    // position moved is what keeps it visually under the cursor — the
    // scrollbar clamps this to its valid range automatically if it would
    // otherwise go out of bounds (e.g. zooming out near the very start).
    hbar->setValue(hbar->value() + (newPixelX - oldPixelX));
}

void MainWindow::onClipDeleted() {
    scheduleTranscriptionScan(); // a source file may have left the track entirely
    // Re-sync immediately (not just on the next tick) since one of the
    // deleted clips might have been what's currently loaded/shown, and
    // this should take effect even while paused.
    seekTimeline(m_currentTimelineSec);
}

void MainWindow::onExportClicked() {
    if (m_project.durationSec() <= 0.0) {
        QMessageBox::information(this, "Nothing to export",
            "Add some clips to the timeline first.");
        return;
    }

    // Fail on the missing dependency BEFORE asking where to save. Making someone
    // pick a filename and then telling them it can't be written is the wrong
    // order to discover that in.
    if (FFmpegExporter::resolveFfmpegPath().isEmpty()) {
        QMessageBox::warning(this, "ffmpeg not found",
            "Exporting needs the ffmpeg program, which couldn't be found.\n\n"
            "Put ffmpeg.exe next to this application, or install it and make sure it's "
            "on your PATH, then try again.");
        return;
    }

    const QString outPath = QFileDialog::getSaveFileName(
        this, "Export Video", QString(), "MP4 video (*.mp4)");
    if (outPath.isEmpty()) return;

    FFmpegExporter::Options options;
    options.outputPath = outPath;

    // Default the canvas to the first video clip's own resolution and frame
    // rate, so the common case of "render this back out as it came in" needs no
    // decisions at all.
    for (const auto& track : m_project.tracks) {
        if (track.type != TrackType::Video || track.clips.isEmpty()) continue;
        const auto info = MediaProbe::probeVideoInfo(track.clips.first().sourcePath);
        if (info.valid()) {
            options.width = info.width;
            options.height = info.height;
            if (info.fps > 1.0 && info.fps < 1000.0) options.fps = info.fps;
        }
        break;
    }

    QProgressDialog progress("Rendering…", "Cancel", 0, 100, this);
    progress.setWindowTitle("Export");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setValue(0);

    QElapsedTimer elapsed;
    elapsed.start();

    FFmpegExporter exporter;
    const bool ok = exporter.exportProject(m_project, options, [&](double fraction) {
        progress.setValue(static_cast<int>(fraction * 100));

        // A time remaining estimate, once there's enough of a sample for it not
        // to be nonsense. A render is long enough that "43% done" alone doesn't
        // answer the question people are actually asking.
        if (fraction > 0.03) {
            const double totalMs = elapsed.elapsed() / fraction;
            const int remainingSec = static_cast<int>((totalMs - elapsed.elapsed()) / 1000.0);
            progress.setLabelText(QString("Rendering…  %1%   ·   about %2 remaining")
                .arg(static_cast<int>(fraction * 100))
                .arg(remainingSec >= 60
                        ? QString("%1 min").arg((remainingSec + 30) / 60)
                        : QString("%1 sec").arg(std::max(1, remainingSec))));
        }

        // Keeps the dialog responsive: the export loop blocks on ffmpeg's output,
        // so without pumping events here the Cancel button would never repaint,
        // let alone register a click.
        QApplication::processEvents();
        return !progress.wasCanceled();
    });

    progress.close();

    if (!ok) {
        if (exporter.errorMessage() == "Export cancelled.") return; // their own choice; no alarm needed

        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle("Export failed");
        box.setText("The video couldn't be rendered.");
        box.setInformativeText(exporter.errorMessage());
        // The graph is the single most useful thing for diagnosing a failed
        // render, but it's noise for anyone who just wants to know it broke —
        // hence tucked into Details rather than shown outright.
        box.setDetailedText("Filter graph:\n\n" + exporter.lastFilterGraph());
        box.exec();
        return;
    }

    QMessageBox::information(this, "Export complete",
        QString("Rendered to:\n%1").arg(QDir::toNativeSeparators(outPath)));
}


QString MainWindow::resolveWhisperModelPath(bool allowPrompt) {
    QSettings settings;
    const QString saved = settings.value("whisperModelPath").toString();
    if (!saved.isEmpty() && QFileInfo::exists(saved)) return saved;

    const QString conventional = QCoreApplication::applicationDirPath() + "/models/ggml-base.en.bin";
    if (QFileInfo::exists(conventional)) {
        settings.setValue("whisperModelPath", conventional);
        return conventional;
    }

    // Automatic transcription calls this with allowPrompt=false: it runs on its
    // own initiative, and a background task is never entitled to interrupt with
    // a modal file picker.
    if (!allowPrompt) return QString();

    const QString chosen = QFileDialog::getOpenFileName(this, "Locate Whisper Model",
        QString(), "Whisper GGML model (*.bin)");
    if (!chosen.isEmpty()) {
        settings.setValue("whisperModelPath", chosen);
    }
    return chosen;
}

QString MainWindow::transcriptSignatureFor(const Track& track) const {
    QStringList sources;
    for (const Clip& clip : track.clips) {
        if (!clip.sourcePath.isEmpty() && !sources.contains(clip.sourcePath)) {
            sources << clip.sourcePath;
        }
    }
    sources.sort(); // order on the timeline is irrelevant to what needs transcribing
    return sources.join(QChar(0x1F)); // unit separator — cannot occur in a path
}

void MainWindow::scheduleTranscriptionScan() {
    // Debounced rather than immediate. Dropping several files in a row, or
    // deleting a clip and adding another, would otherwise each kick off a
    // multi-minute whisper run that the next edit immediately invalidates.
    if (!m_transcriptScanTimer) {
        m_transcriptScanTimer = new QTimer(this);
        m_transcriptScanTimer->setSingleShot(true);
        m_transcriptScanTimer->setInterval(1200);
        connect(m_transcriptScanTimer, &QTimer::timeout, this, &MainWindow::scanForTranscriptionWork);
    }
    m_transcriptScanTimer->start();
}

void MainWindow::scanForTranscriptionWork() {
    // No model, no work — and deliberately no dialog. Surfacing a button in the
    // transcript panel lets someone who has no interest in transcription simply
    // ignore it, rather than being interrupted by a file picker.
    if (resolveWhisperModelPath(/*allowPrompt=*/false).isEmpty()) {
        bool anyAudio = false;
        for (const auto& track : m_project.tracks) {
            if (track.type == TrackType::Audio && !track.clips.isEmpty()) { anyAudio = true; break; }
        }
        if (m_chooseModelButton) m_chooseModelButton->setVisible(anyAudio);
        return;
    }
    if (m_chooseModelButton) m_chooseModelButton->hide();

    for (int t = 0; t < m_project.tracks.size(); ++t) {
        Track& track = m_project.tracks[t];
        if (track.type != TrackType::Audio) continue;

        const QString signature = transcriptSignatureFor(track);

        if (signature.isEmpty()) {
            // Every clip gone — drop the transcript with them rather than
            // leaving text on screen describing audio that isn't there.
            if (!track.transcript.isEmpty() || !track.transcriptSignature.isEmpty()) {
                track.transcript.clear();
                track.transcriptSignature.clear();
                refreshTranscriptTab(t);
            }
            continue;
        }

        if (track.transcriptSignature == signature) continue; // already current

        // Don't queue the same track twice if a scan lands while its jobs are
        // still pending.
        bool alreadyQueued = false;
        for (const auto& job : m_transcriptionQueue) {
            if (job.trackIndex == t) { alreadyQueued = true; break; }
        }
        if (alreadyQueued) continue;

        bool first = true;
        for (const QString& source : signature.split(QChar(0x1F))) {
            if (source.isEmpty()) continue;
            m_transcriptionQueue.push_back({t, source, first});
            first = false;
        }
    }

    startNextTranscriptionJob();
}

void MainWindow::startNextTranscriptionJob() {
    if (m_transcriptionRunning || m_transcriptionQueue.isEmpty()) return;

    const TranscriptionJob job = m_transcriptionQueue.takeFirst();
    if (job.trackIndex >= m_project.tracks.size()
        || m_project.tracks[job.trackIndex].type != TrackType::Audio) {
        startNextTranscriptionJob(); // track vanished while queued
        return;
    }

    const QString modelPath = resolveWhisperModelPath(/*allowPrompt=*/false);
    if (modelPath.isEmpty()) { m_transcriptionQueue.clear(); return; }

    if (job.firstForTrack) {
        m_project.tracks[job.trackIndex].transcript.clear();
        refreshTranscriptTab(job.trackIndex);
    }

    m_transcriptionRunning = true;
    m_transcribeStatusLabel->setText(
        QString("Transcribing %1").arg(QFileInfo(job.sourcePath).fileName()));
    m_transcribeStatusLabel->show();
    m_transcribeProgressBar->setValue(0);
    m_transcribeProgressBar->show();
    // NOTE: whisper.cpp only reports progress DURING inference — the bar sits at
    // 0 briefly first while FFmpeg decodes the file's audio, since that earlier
    // phase has no progress signal of its own.

    // Runs entirely on a background thread — decoding a whole file's audio plus
    // whisper inference is genuinely slow (real minutes on CPU), and neither
    // FFmpeg decode nor whisper.cpp touch any Qt GUI objects.
    auto* watcher = new QFutureWatcher<QVector<TranscriptSegment>>(this);
    connect(watcher, &QFutureWatcher<QVector<TranscriptSegment>>::progressValueChanged,
            this, [this](int value) { m_transcribeProgressBar->setValue(value); });

    connect(watcher, &QFutureWatcher<QVector<TranscriptSegment>>::finished, this,
            [this, watcher, job] {
        QVector<TranscriptSegment> segments = watcher->result();
        watcher->deleteLater();

        m_transcriptionRunning = false;
        m_transcribeProgressBar->hide();
        m_transcribeStatusLabel->hide();

        if (job.trackIndex < m_project.tracks.size()
            && m_project.tracks[job.trackIndex].type == TrackType::Audio) {
            Track& track = m_project.tracks[job.trackIndex];

            // Tagging each segment with the file it came from is what lets one
            // track hold transcripts of several different clips and still map
            // click-to-seek back to the right one.
            for (auto& segment : segments) segment.sourcePath = job.sourcePath;
            track.transcript += segments;

            // Ordered by where each segment actually lands on the timeline, so
            // the panel reads top-to-bottom in the order you'd hear it — not
            // grouped by whichever file happened to be transcribed first.
            std::sort(track.transcript.begin(), track.transcript.end(),
                      [this, &job](const TranscriptSegment& a, const TranscriptSegment& b) {
                const double ta = mapSourceTimeToTimelineSec(job.trackIndex, a.sourcePath, a.startSec);
                const double tb = mapSourceTimeToTimelineSec(job.trackIndex, b.sourcePath, b.startSec);
                if (ta < 0 || tb < 0) return a.startSec < b.startSec; // trimmed away — keep source order
                return ta < tb;
            });

            // Only once every job for this track has been consumed is the
            // transcript actually complete, so that's when the signature is
            // recorded. Marking it earlier would let a half-finished transcript
            // look up to date.
            bool moreForTrack = false;
            for (const auto& queued : m_transcriptionQueue) {
                if (queued.trackIndex == job.trackIndex) { moreForTrack = true; break; }
            }
            if (!moreForTrack) track.transcriptSignature = transcriptSignatureFor(track);

            refreshTranscriptTab(job.trackIndex);
            if (!m_transcriptSearchBox->text().isEmpty()) {
                onTranscriptSearchTextChanged(m_transcriptSearchBox->text());
            }
        }

        startNextTranscriptionJob();
    });

    watcher->setFuture(QtConcurrent::run(
        [modelPath, source = job.sourcePath](QPromise<QVector<TranscriptSegment>>& promise) {
        promise.setProgressRange(0, 100);
        Transcriber transcriber(modelPath);
        transcriber.setProgressCallback([&promise](int percent) {
            if (!promise.isCanceled()) promise.setProgressValue(percent);
        });
        promise.addResult(transcriber.transcribe(source));
    }));
}

void MainWindow::onSplitClicked() {
    const QVector<QPair<int, int>> selected = m_timeline->selectedClips();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "Split", "Select one or more clips on the timeline first.");
        return;
    }

    // Group by track, then process each track's selected clips in
    // DESCENDING index order — splitClipAt inserts a new clip right after
    // the one it splits, which would otherwise shift the indices of any
    // other selected (higher-index) clips on the same track still pending.
    QHash<int, QVector<int>> byTrack;
    for (const auto& pair : selected) {
        byTrack[pair.first].push_back(pair.second);
    }

    bool anySplit = false;
    for (auto it = byTrack.begin(); it != byTrack.end(); ++it) {
        std::sort(it.value().begin(), it.value().end(), std::greater<int>());
        for (int clipIdx : it.value()) {
            if (m_project.splitClipAt(it.key(), clipIdx, m_currentTimelineSec)) {
                anySplit = true;
            }
        }
    }

    if (anySplit) {
        m_timeline->setProject(&m_project);
    }
}

void MainWindow::onZoomInClicked() {
    m_timeline->setPixelsPerSecond(m_timeline->pixelsPerSecond() * kZoomFactor);
}

void MainWindow::onZoomOutClicked() {
    m_timeline->setPixelsPerSecond(m_timeline->pixelsPerSecond() / kZoomFactor);
}

void MainWindow::onZoomToFitClicked() {
    m_timeline->zoomToFit(m_timelineScrollArea->viewport()->width());
}

void MainWindow::onAddVideoTrackClicked() {
    int videoCount = 0, audioCount = 0;
    for (const auto& t : m_project.tracks) {
        if (t.type == TrackType::Video) ++videoCount;
        else ++audioCount;
    }

    m_project.addTrack(TrackType::Video, QString("Video %1").arg(videoCount + 1));
    const int newVideoIndex = m_project.tracks.size() - 1;

    // A dedicated paired audio track is created alongside it — otherwise
    // importing a video onto this track has nowhere sensible to route its
    // companion audio (previously it silently fell back to Audio 1 always,
    // regardless of which video track was actually targeted).
    m_project.addTrack(TrackType::Audio, QString("Audio %1").arg(audioCount + 1));
    const int newAudioIndex = m_project.tracks.size() - 1;
    setupAudioPlayerForTrack(newAudioIndex);

    m_project.tracks[newVideoIndex].pairedAudioTrackIndex = newAudioIndex;

    refreshTrackViews();

    // NOTE: this new video track DOES appear in the Preview player now —
    // see the priority-layering note on syncVideoToTimeline in
    // MainWindow.h. It'll show through as an overlay wherever it has a
    // clip, falling back to earlier video tracks elsewhere.
}

void MainWindow::onAddAudioTrackClicked() {
    int audioCount = 0;
    for (const auto& t : m_project.tracks) {
        if (t.type == TrackType::Audio) ++audioCount;
    }
    m_project.addTrack(TrackType::Audio, QString("Audio %1").arg(audioCount + 1));
    const int newTrackIndex = m_project.tracks.size() - 1;
    setupAudioPlayerForTrack(newTrackIndex); // gets its own independent AudioPlayer immediately

    refreshTrackViews();
}

void MainWindow::onPlayerFileLoaded(double /*durationSeconds*/) {
    // loadFile() is async — this is where a pending seek queued by
    // syncVideoToTimeline() actually gets applied, once mpv confirms the
    // file is ready to seek within.
    if (m_awaitingSeekAfterLoad) {
        m_player->seek(m_pendingSeekSec);
        m_awaitingSeekAfterLoad = false;
    }
    // mpv autoplays on load by default — respect current pause/play intent
    // rather than always starting playback on a freshly loaded file.
    if (!m_isPlayingIntent) {
        m_player->pause();
    }
}

void MainWindow::onUserToggledPlayback(bool nowPlaying) {
    m_isPlayingIntent = nowPlaying;
    if (nowPlaying) {
        m_masterClockElapsed.restart();
        m_masterClockTimer->start();
    } else {
        m_masterClockTimer->stop();
    }
    // Apply immediately rather than waiting for the next tick — important
    // when pausing, since the master clock (and thus future ticks) just
    // stopped, so nothing else would bring the players to a matching state.
    syncVideoToTimeline(m_currentTimelineSec);
    syncAudioTracksToTimeline(m_currentTimelineSec);
}

void MainWindow::onUserRequestedSkip(double deltaSeconds) {
    seekTimeline(m_currentTimelineSec + deltaSeconds);
}

void MainWindow::onUserRequestedGoToStart() {
    seekTimeline(0.0);
}

void MainWindow::onUserRequestedGoToEnd() {
    seekTimeline(m_project.durationSec());
}

int MainWindow::combinedVolumeForTrack(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= m_project.tracks.size()) return m_masterVolumePercent;
    return (m_masterVolumePercent * m_project.tracks[trackIndex].volumePercent) / 100;
}

void MainWindow::onVolumeChanged(int percent) {
    m_masterVolumePercent = percent;
    for (auto& audio : m_audioTracks) {
        audio.player->setVolume(combinedVolumeForTrack(audio.trackIndex));
    }
}

void MainWindow::onTrackVolumeChanged(int trackIndex) {
    for (auto& audio : m_audioTracks) {
        if (audio.trackIndex == trackIndex) {
            audio.player->setVolume(combinedVolumeForTrack(trackIndex));
            break;
        }
    }
}

void MainWindow::onTimelineSeekRequested(double timelineSeconds) {
    seekTimeline(timelineSeconds);
}

void MainWindow::onMasterClockTick() {
    const double deltaSec = m_masterClockElapsed.restart() / 1000.0;
    m_currentTimelineSec += deltaSec;
    m_timeline->setPlayheadSec(m_currentTimelineSec);
    syncVideoToTimeline(m_currentTimelineSec);
    syncAudioTracksToTimeline(m_currentTimelineSec);
    syncOverlaysToTimeline(m_currentTimelineSec);
}

void MainWindow::seekTimeline(double timelineSeconds) {
    m_currentTimelineSec = std::max(0.0, timelineSeconds);
    m_timeline->setPlayheadSec(m_currentTimelineSec);
    if (m_overlayInspector) m_overlayInspector->setPlayheadSec(m_currentTimelineSec);
    if (m_overlayStage) m_overlayStage->setPlayheadSec(m_currentTimelineSec);
    syncVideoToTimeline(m_currentTimelineSec);
    syncAudioTracksToTimeline(m_currentTimelineSec);
    syncOverlaysToTimeline(m_currentTimelineSec);
}

void MainWindow::syncVideoToTimeline(double timelineSeconds) {
    if (m_project.tracks.isEmpty()) return;

    // Priority-based layering: check video tracks in REVERSE index order
    // (later/lower-in-the-list tracks first) and use the first one that
    // actually has a clip covering this position. A track with nothing
    // here is "transparent" — the check falls through to the track above
    // it. See the architecture note on this method in MainWindow.h for
    // what this does and doesn't achieve (visibility priority, not true
    // alpha blending).
    int winningTrackIndex = -1;
    int winningClipIndex = -1;
    for (int t = m_project.tracks.size() - 1; t >= 0; --t) {
        if (m_project.tracks[t].type != TrackType::Video || !m_project.tracks[t].enabled) continue;
        const int clipIdx = findClipIndexAt(m_project.tracks[t], timelineSeconds);
        if (clipIdx >= 0) {
            winningTrackIndex = t;
            winningClipIndex = clipIdx;
            break;
        }
    }

    if (winningTrackIndex < 0) {
        // No video track has anything at this position at all — real black
        // screen (not just "paused on the last frame"), re-evaluated live
        // on every tick, so a clip dragged into or out of this span is
        // picked up immediately rather than trusting a stale snapshot.
        m_player->pause();
        m_player->setBlackout(true);
        m_playingVideoTrackIndex = -1;
        m_playingClipIndex = -1;
        return;
    }

    m_player->setBlackout(false);
    const Clip& clip = m_project.tracks[winningTrackIndex].clips[winningClipIndex];
    const double expectedSourceSec = clip.sourceInSec + (timelineSeconds - clip.trackPosSec);
    // Only reload when the FILE actually differs — cutting a clip creates
    // two clips sharing the same sourcePath, so clipIdx changes at every
    // cut even though there's nothing new to open. Reloading on index
    // change alone forced a full loadFile() (which resets to frame 0) at
    // every single cut, even within one continuous source file. Also
    // reload if the WINNING TRACK changed (a different track's clip has
    // taken over at this position) even if the clip index coincidentally
    // matches — the track+clip pair, not clip index alone, identifies which
    // clip is actually meant.
    const bool needsLoad = (m_currentLoadedPath != clip.sourcePath)
                         || (m_playingVideoTrackIndex != winningTrackIndex);
    m_playingVideoTrackIndex = winningTrackIndex;
    m_playingClipIndex = winningClipIndex;

    if (needsLoad) {
        m_currentLoadedPath = clip.sourcePath;
        m_pendingSeekSec = expectedSourceSec;
        m_awaitingSeekAfterLoad = true;
        m_player->loadFile(clip.sourcePath);
    } else {
        // Self-correcting drift check rather than seeking every tick — also
        // what transparently handles rewind/fast-forward and any other
        // abrupt jump: it just shows up as drift and gets corrected within
        // one tick (~16ms).
        //
        // Threshold raised from 0.15s and correction switched to
        // non-exact (keyframe) seeking: exact/frame-accurate seeks are
        // genuinely expensive to compute on files with sparser keyframes,
        // and doing one every time drift exceeds a TIGHT threshold was
        // itself causing periodic stutter. Raised again to 0.75s — this
        // only needs to catch REAL jumps (explicit skips/seeks, or crossing
        // into the next clip at a same-file cut, both of which produce
        // large jumps well above this threshold); mpv naturally stays in
        // sync during uninterrupted continuous playback, so there's no
        // benefit to correcting sub-second drift, only cost.
        const double drift = std::fabs(m_player->positionSec() - expectedSourceSec);
        if (drift > 0.75) {
            m_player->seek(expectedSourceSec, /*exact=*/false);
        }
    }

    if (m_isPlayingIntent && m_player->isPaused()) m_player->play();
    if (!m_isPlayingIntent && !m_player->isPaused()) m_player->pause();
}

QImage MainWindow::renderOverlayBitmap(int trackIndex, int clipIndex, const Clip& clip,
                                       double localSec, const QSize& canvas, QPoint* outPos) {
    const QImage source = OverlayImageLoader::load(clip.sourcePath);
    if (source.isNull() || source.width() <= 0) return QImage();

    const double scale   = std::clamp(clip.anim.scale.valueAt(localSec), 0.005, 4.0);
    const double rotationDeg = clip.anim.rotation.valueAt(localSec);
    const double opacity = std::clamp(clip.anim.opacity.valueAt(localSec), 0.0, 1.0);
    const double xNorm   = clip.anim.x.valueAt(localSec);
    const double yNorm   = clip.anim.y.valueAt(localSec);

    // Width is a fraction of the canvas; height follows the image's own aspect,
    // so an overlay never distorts regardless of the output resolution.
    const int targetW = std::max(2, static_cast<int>(canvas.width() * scale));
    const int targetH = std::max(2, static_cast<int>(
        static_cast<double>(targetW) * source.height() / source.width()));
    const int opacityStep = static_cast<int>(opacity * 255.0 + 0.5);
    // Quantised to whole degrees: re-rendering for a hundredth of a degree of
    // change would defeat the cache without being visible.
    const int rotationStep = static_cast<int>(std::lround(rotationDeg));

    OverlayRenderCache& cache = m_overlayCacheByTrack[trackIndex];
    // Smooth scaling of a large PNG is the single most expensive thing in the
    // per-frame overlay path, and while a handle is being dragged it runs on
    // every mouse move. Dropping to fast scaling for the duration keeps the
    // overlay glued to the pointer; the quality is part of the cache key, so
    // releasing the handle re-renders it properly.
    const bool fast = m_overlayStage && m_overlayStage->isDragging();
    const Qt::TransformationMode mode = fast ? Qt::FastTransformation : Qt::SmoothTransformation;

    const bool cacheValid = !cache.rendered.isNull()
                         && cache.clipIndex == clipIndex
                         && cache.width == targetW
                         && cache.height == targetH
                         && cache.opacityStep == opacityStep
                         && cache.rotationStep == rotationStep
                         && cache.fastQuality == fast;

    if (!cacheValid) {
        QImage scaled = source.scaled(targetW, targetH, Qt::IgnoreAspectRatio, mode)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (opacityStep < 255) {
            // DestinationIn multiplies the destination by the source's alpha,
            // which scales any existing per-pixel transparency rather than
            // flattening a PNG's own alpha into a uniform value. In a
            // premultiplied image it scales the colour components alongside the
            // alpha, which is exactly what keeps the result valid for mpv.
            QPainter fade(&scaled);
            fade.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            fade.fillRect(scaled.rect(), QColor(0, 0, 0, opacityStep));
        }

        if (std::abs(rotationDeg) > 0.01) {
            // Rotating about the centre grows the bitmap to fit the corners,
            // exactly as the export's rotate filter does. Both then position by
            // the centre, so the two stay in agreement without either needing to
            // know the other's padding.
            QTransform transform;
            transform.rotate(rotationDeg);
            scaled = scaled.transformed(transform, mode);
        }

        cache.clipIndex = clipIndex;
        cache.width = targetW;
        cache.height = targetH;
        cache.opacityStep = opacityStep;
        cache.rotationStep = rotationStep;
        cache.fastQuality = fast;
        cache.rendered = scaled;
    }

    if (outPos) {
        // x/y address the overlay's CENTRE, which is what makes a "slide in from
        // the left" animation behave the same way regardless of the image's size —
        // and, once rotation is in play, what keeps it spinning in place rather
        // than swinging around its top-left corner.
        *outPos = QPoint(static_cast<int>(canvas.width() * xNorm) - cache.rendered.width() / 2,
                         static_cast<int>(canvas.height() * yNorm) - cache.rendered.height() / 2);
    }
    return cache.rendered;
}

void MainWindow::syncOverlaysToTimeline(double timelineSeconds) {
    const QSize canvas = m_player->overlayCanvasSize();

    for (int t = 0; t < m_project.tracks.size(); ++t) {
        if (m_project.tracks[t].type != TrackType::Overlay) continue;

        const int overlayId = t + 1; // stable per-track id (mpv ids start meaningfully at 0, avoid 0 to keep debugging simpler)
        const int currentlyShown = m_activeOverlayClipByTrack.value(t, -1);

        auto hideOverlay = [&] {
            if (currentlyShown != -1) {
                m_player->clearOverlay(overlayId);
                m_activeOverlayClipByTrack[t] = -1;
                m_overlayCacheByTrack.remove(t);
            }
        };

        if (!m_project.tracks[t].enabled) { hideOverlay(); continue; }

        const int clipIdx = findClipIndexAt(m_project.tracks[t], timelineSeconds);
        if (clipIdx < 0) { hideOverlay(); continue; }

        const Clip& clip = m_project.tracks[t].clips[clipIdx];

        // Unlike before, an unchanged clip index is NOT enough to skip the work:
        // the whole point of animation is that the same clip looks different from
        // one moment to the next. The expensive part (rescaling, alpha) is guarded
        // by the render cache instead, so a static overlay still costs almost
        // nothing while an animated one updates every tick.
        const double localSec = timelineSeconds - clip.trackPosSec;

        QPoint pos;
        const QImage bitmap = renderOverlayBitmap(t, clipIdx, clip, localSec, canvas, &pos);
        if (bitmap.isNull()) { hideOverlay(); continue; }

        // Fully transparent: drop the overlay outright rather than pushing an
        // invisible buffer, so a clip that fades to zero really does stop costing
        // anything to composite.
        if (clip.anim.opacity.valueAt(localSec) <= 0.002) { hideOverlay(); continue; }

        m_player->setOverlay(overlayId, bitmap, pos.x(), pos.y());
        m_activeOverlayClipByTrack[t] = clipIdx;
    }
}


void MainWindow::syncAudioTracksToTimeline(double timelineSeconds) {
    for (auto& audio : m_audioTracks) {
        const Track& track = m_project.tracks[audio.trackIndex];
        audio.player->setMuted(track.muted); // cheap/idempotent — just keep it in sync every tick
        audio.player->setVolume(combinedVolumeForTrack(audio.trackIndex)); // same — cheap, keeps master+per-track volume correct continuously

        if (!track.enabled) {
            // Disabled is stronger than muted — skip this track entirely,
            // as if it had no clips at all.
            if (!audio.player->isPaused()) audio.player->pause();
            audio.playingClipIndex = -1;
            continue;
        }

        const int clipIdx = findClipIndexAt(track, timelineSeconds);

        if (clipIdx < 0) {
            // This track's own gap — silence it, regardless of master
            // intent or what the video track is doing.
            if (!audio.player->isPaused()) audio.player->pause();
            audio.playingClipIndex = -1;
            continue;
        }

        const Clip& clip = track.clips[clipIdx];
        const double expectedSourceSec = clip.sourceInSec + (timelineSeconds - clip.trackPosSec);
        // Same fix as syncVideoToTimeline: only reload on an actual file
        // change, not merely a different clip index (which changes at
        // every cut even within one continuous source file).
        const bool needsLoad = (audio.currentLoadedPath != clip.sourcePath);
        audio.playingClipIndex = clipIdx;

        if (needsLoad) {
            audio.currentLoadedPath = clip.sourcePath;
            audio.pendingSeekSec = expectedSourceSec;
            audio.awaitingSeekAfterLoad = true;
            audio.player->loadFile(clip.sourcePath);
        } else {
            // Same reasoning as syncVideoToTimeline — raised to 0.75s.
            const double drift = std::fabs(audio.player->positionSec() - expectedSourceSec);
            if (drift > 0.75) {
                audio.player->seek(expectedSourceSec, /*exact=*/false);
            }
        }

        if (m_isPlayingIntent && audio.player->isPaused()) audio.player->play();
        if (!m_isPlayingIntent && !audio.player->isPaused()) audio.player->pause();
    }
}

int MainWindow::findClipIndexAt(const Track& track, double timelineSeconds) const {
    for (int i = 0; i < track.clips.size(); ++i) {
        const Clip& c = track.clips[i];
        if (timelineSeconds >= c.trackPosSec && timelineSeconds < c.trackPosSec + c.durationSec()) {
            return i;
        }
    }
    return -1;
}

void MainWindow::onClipSelected(int /*trackIndex*/, int /*clipIndex*/) {
    // Informational only — Timeline owns selection state itself (including
    // multi-select), and Split queries m_timeline->selectedClips() directly
    // rather than MainWindow tracking a parallel copy that could drift out
    // of sync with multi-selection changes.
}

void MainWindow::onTranscriptWordClicked(int trackIndex, int segmentIndex, int wordIndex) {
    if (trackIndex < 0 || trackIndex >= m_project.tracks.size()) return;
    const Track& track = m_project.tracks[trackIndex];
    const auto& transcript = track.transcript;
    if (segmentIndex < 0 || segmentIndex >= transcript.size()) return;
    const auto& seg = transcript[segmentIndex];
    if (wordIndex < 0 || wordIndex >= seg.words.size()) return;

    const double sourceTimeSec = seg.words[wordIndex].startSec;
    // The segment's own source, not a single per-track one: a track can hold
    // clips from several files and each was transcribed separately.
    const QString segmentSource = track.transcript[segmentIndex].sourcePath.isEmpty()
        ? track.transcriptSourcePath // fall back for transcripts made before this was per-segment
        : track.transcript[segmentIndex].sourcePath;
    const double timelineSec = mapSourceTimeToTimelineSec(trackIndex, segmentSource, sourceTimeSec);

    if (timelineSec >= 0.0) {
        // seekTimeline (not a direct player seek) is what actually moves
        // the Timeline's needle — it updates m_currentTimelineSec (the
        // single source of truth the whole playback architecture treats as
        // authoritative) and calls Timeline::setPlayheadSec, then re-syncs
        // video/audio/overlays consistently from there.
        seekTimeline(timelineSec);
    } else {
        // This moment isn't currently kept on the timeline at all (trimmed
        // away since transcribing, or the clip was removed) — there's
        // nowhere meaningful for the needle to point, so this is a
        // best-effort fallback: still move the player itself rather than
        // leaving the click feeling like a dead no-op.
        m_player->seek(sourceTimeSec);
    }
}

double MainWindow::mapSourceTimeToTimelineSec(int trackIndex, const QString& sourcePath, double sourceTimeSec) const {
    if (trackIndex < 0 || trackIndex >= m_project.tracks.size()) return -1.0;
    const Track& track = m_project.tracks[trackIndex];
    for (const Clip& clip : track.clips) {
        if (clip.sourcePath != sourcePath) continue;
        if (sourceTimeSec >= clip.sourceInSec && sourceTimeSec <= clip.sourceOutSec) {
            return clip.trackPosSec + (sourceTimeSec - clip.sourceInSec);
        }
    }
    return -1.0; // no current clip covers this moment
}

QString MainWindow::formatTranscriptTimestamp(double seconds) const {
    const int totalSecs = static_cast<int>(seconds);
    const int hours = totalSecs / 3600;
    const int minutes = (totalSecs % 3600) / 60;
    const int secs = totalSecs % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

void MainWindow::populateTranscriptList(QListWidget* list, const QVector<TranscriptSegment>& segments) {
    list->clear();

    // An empty transcript panel used to be a blank white-on-dark void with no
    // indication of what it was for or how to fill it. An empty state should be
    // an invitation to act, so it now says what's missing and what to do.
    if (segments.isEmpty()) {
        auto* placeholder = new QListWidgetItem(
            "No transcript yet.\n\nAdd audio to this track and it will be transcribed "
            "automatically, and kept up to date as the audio changes.", list);
        placeholder->setFlags(Qt::NoItemFlags); // not selectable, not clickable — it isn't a segment
        placeholder->setForeground(QBrush(Theme::textFaint()));
        return;
    }

    for (int i = 0; i < segments.size(); ++i) {
        const QString timestamp = formatTranscriptTimestamp(segments[i].startSec);
        auto* item = new QListWidgetItem(QString("%1   %2").arg(timestamp, segments[i].text), list);
        item->setData(Qt::UserRole, i); // segment index within THIS track's transcript
        item->setToolTip("Double-click to jump the playhead here");
    }
}

void MainWindow::rebuildTranscriptTabs() {
    // Remember which track's tab was active so the rebuild doesn't feel
    // disruptive if it's triggered by something incidental (a rename
    // elsewhere, adding an unrelated track).
    const int previousActiveTrackIndex = currentTranscriptTrackIndex();

    m_transcriptTabs->clear(); // deletes the old page widgets too — safe since we recreate them below
    m_transcriptListByTrack.clear();

    for (int i = 0; i < m_project.tracks.size(); ++i) {
        if (m_project.tracks[i].type != TrackType::Audio) continue;

        auto* list = new QListWidget();
        // Segments are full sentences and the panel is narrow — without
        // wrapping they ran off the right edge and forced a horizontal
        // scrollbar across the whole transcript.
        list->setWordWrap(true);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setSpacing(1);
        list->setFont(Theme::monoFont(-1));
        connect(list, &QListWidget::itemDoubleClicked, this, [this, i](QListWidgetItem* item) {
            // Seeks to the segment's FIRST word — a simple but functional
            // click-to-seek; onTranscriptWordClicked already supports true
            // per-word granularity for a future custom item delegate that
            // renders individual clickable words within a segment.
            onTranscriptWordClicked(i, item->data(Qt::UserRole).toInt(), 0);
        });
        populateTranscriptList(list, m_project.tracks[i].transcript);
        m_transcriptListByTrack[i] = list;

        const int tabIndex = m_transcriptTabs->addTab(list, m_project.tracks[i].name);
        m_transcriptTabs->tabBar()->setTabData(tabIndex, i); // remember which track index this tab represents

        if (i == previousActiveTrackIndex) {
            m_transcriptTabs->setCurrentIndex(tabIndex);
        }
    }
}

void MainWindow::refreshTranscriptTab(int trackIndex) {
    if (!m_transcriptListByTrack.contains(trackIndex)) {
        rebuildTranscriptTabs(); // tab doesn't exist yet (shouldn't normally happen) — safety net
        return;
    }

    populateTranscriptList(m_transcriptListByTrack[trackIndex], m_project.tracks[trackIndex].transcript);

    for (int i = 0; i < m_transcriptTabs->count(); ++i) {
        if (m_transcriptTabs->tabBar()->tabData(i).toInt() == trackIndex) {
            m_transcriptTabs->setCurrentIndex(i); // jump to it so the fresh result is immediately visible
            break;
        }
    }
}

int MainWindow::currentTranscriptTrackIndex() const {
    const int tabIndex = m_transcriptTabs->currentIndex();
    if (tabIndex < 0) return -1;
    return m_transcriptTabs->tabBar()->tabData(tabIndex).toInt();
}

QListWidget* MainWindow::currentTranscriptList() const {
    return qobject_cast<QListWidget*>(m_transcriptTabs->currentWidget());
}

void MainWindow::showTranscriptSearch() {
    m_transcriptDock->show();
    m_transcriptDock->raise();
    m_transcriptSearchBox->setFocus();
    m_transcriptSearchBox->selectAll();
}

void MainWindow::onTranscriptSearchTextChanged(const QString& text) {
    m_transcriptSearchMatches.clear();
    m_transcriptSearchCurrentMatch = -1;

    QListWidget* list = currentTranscriptList();
    if (!list) return; // no tabs exist yet (no audio tracks) — nothing to search

    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* item = list->item(i);
        const bool matches = !text.isEmpty() && item->text().contains(text, Qt::CaseInsensitive);
        item->setBackground(matches ? QBrush(QColor(Theme::accent().red(), Theme::accent().green(),
                                                                   Theme::accent().blue(), 55))
                                                : QBrush());
        if (matches) m_transcriptSearchMatches.push_back(i);
    }

    if (!m_transcriptSearchMatches.isEmpty()) {
        m_transcriptSearchCurrentMatch = 0;
        list->scrollToItem(list->item(m_transcriptSearchMatches[0]));
        list->setCurrentRow(m_transcriptSearchMatches[0]);
    }
}

void MainWindow::onTranscriptSearchNext() {
    QListWidget* list = currentTranscriptList();
    if (!list || m_transcriptSearchMatches.isEmpty()) return;
    m_transcriptSearchCurrentMatch = (m_transcriptSearchCurrentMatch + 1) % m_transcriptSearchMatches.size();
    const int row = m_transcriptSearchMatches[m_transcriptSearchCurrentMatch];
    list->scrollToItem(list->item(row));
    list->setCurrentRow(row);
    // also jump the playhead there
    onTranscriptWordClicked(currentTranscriptTrackIndex(), list->item(row)->data(Qt::UserRole).toInt(), 0);
}

void MainWindow::onTranscriptSearchPrev() {
    QListWidget* list = currentTranscriptList();
    if (!list || m_transcriptSearchMatches.isEmpty()) return;
    m_transcriptSearchCurrentMatch = (m_transcriptSearchCurrentMatch - 1 + m_transcriptSearchMatches.size())
                                     % m_transcriptSearchMatches.size();
    const int row = m_transcriptSearchMatches[m_transcriptSearchCurrentMatch];
    list->scrollToItem(list->item(row));
    list->setCurrentRow(row);
    onTranscriptWordClicked(currentTranscriptTrackIndex(), list->item(row)->data(Qt::UserRole).toInt(), 0);
}

