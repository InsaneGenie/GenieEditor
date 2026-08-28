#pragma once

#include <QMainWindow>
#include <QString>
#include <QElapsedTimer>
#include <QVector>
#include <QHash>
#include <QSet>
#include "Project.h"

class PlayerWidget;
class AudioPlayer;
class Timeline;
class TrackHeaderPanel;
class MediaBrowserPanel;
class OverlayInspectorPanel;
class KlipyPanel;
class OverlayStageWidget;
class QListWidget;
class QTabWidget;
class QLineEdit;
class QProgressBar;
class QLabel;
class QPushButton;
class QAction;
class QDockWidget;
class QScrollArea;
class QCloseEvent;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    // Persists dock/toolbar layout (position, floating, size) so rearranged
    // panels stay put across app restarts.
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onImportClicked();
    void onExportClicked();
    void onSplitClicked();
    void onZoomInClicked();
    void onZoomOutClicked();
    void onZoomToFitClicked();
    void onPlayerFileLoaded(double durationSeconds);
    void onUserToggledPlayback(bool nowPlaying);
    void onUserRequestedSkip(double deltaSeconds);
    void onUserRequestedGoToStart();
    void onUserRequestedGoToEnd();
    void onVolumeChanged(int percent);
    void onTrackVolumeChanged(int trackIndex);
    void onTimelineSeekRequested(double timelineSeconds);
    void onClipSelected(int trackIndex, int clipIndex); // informational only — Split queries Timeline::selectedClips() directly
    void onTranscriptWordClicked(int trackIndex, int segmentIndex, int wordIndex);
    void onTranscriptSearchTextChanged(const QString& text);
    void onTranscriptSearchNext();
    void onTranscriptSearchPrev();
    void onMasterClockTick();
    void onMediaDropped(const QString& filePath, int trackIndex, double timelineSec);
    void onThumbnailDetailNeeded(int trackIndex, int clipIndex, int desiredFullFileFrameCount);
    void onTimelineZoomAnchorChanged(double anchorSec, int oldPixelX);
    void onClipDeleted();

    // --- Project files ------------------------------------------------------
    void onNewProject();
    void onOpenProject();
    bool onSaveProject();      // false if the user cancelled the Save As it may need
    bool onSaveProjectAs();
    void openProjectFile(const QString& path);
    // A move gesture ended with clips in a different track than they started
    // in — see Timeline::clipsMovedBetweenTracks.
    void onClipsMovedBetweenTracks();
    void onAddVideoTrackClicked();
    void onAddAudioTrackClicked();
    void onAddOverlayTrackClicked();
    void onTrackEnabledChanged(int trackIndex);
    void onDeleteTrackRequested(int trackIndex);

private:
    // One of these per audio track, each backed by its own headless mpv
    // instance (AudioPlayer) — see the class comment on AudioPlayer for why
    // a shared instance with video can't give independent per-track audio.
    struct AudioTrackPlayback {
        int trackIndex = -1;
        AudioPlayer* player = nullptr;
        int playingClipIndex = -1;
        QString currentLoadedPath;
        bool awaitingSeekAfterLoad = false;
        double pendingSeekSec = 0.0;

        // A seek is ASYNCHRONOUS: positionSec() keeps reporting the old
        // position until mpv completes it, typically for a good handful of
        // 16ms ticks. Without this, every one of those ticks sees the same
        // uncorrected drift and issues another seek -- around nine of them for
        // a single desync -- and each restarts playback from its target, which
        // is audible as the same fragment of sound playing over and over.
        //
        // Correction is suspended until the seek has had time to land.
        QElapsedTimer sinceSeek;
        bool seekInFlight = false;
    };

    void buildUi();
    void buildMenus();
    void buildStatusBar();
    void restoreLayout();

    // Sets the default dock proportions on first run. Deliberately separate
    // from buildUi and only applied when there's no saved layout to restore —
    // otherwise it would stomp on whatever arrangement the user had last time.
    void applyDefaultLayout();

    // Restores the default arrangement on demand, from the View menu.
    void resetLayout();

    // Bumped whenever the default dock arrangement changes in a way existing
    // users should actually receive — see restoreLayout().
    // Bumped so an existing install picks up the new Sounds dock: a restored
    // older layout describes an arrangement that has no place for it.
    static constexpr int kLayoutVersion = 6;

    // Refreshes the status-bar summary after anything that changes the track
    // list or clip set.
    void updateProjectStats();

    // Points both the inspector and the on-preview handles at the same clip.
    // Kept in one place so the two can never end up editing different things.
    void setOverlaySelection(int trackIndex, int clipIndex);

    // --- Playback architecture --------------------------------------------
    // The TIMELINE POSITION (m_currentTimelineSec) is the single source of
    // truth, advanced by real wall-clock time via a QTimer+QElapsedTimer
    // (onMasterClockTick) whenever m_isPlayingIntent is true — NOT derived
    // from what mpv reports back. On every tick (and on every explicit
    // seek), both the video track and every audio track independently ask
    // "what does the LIVE project say should be playing right now?" and
    // bring their player in line with that.
    //
    // This is deliberately the same pattern audio already used successfully
    // (syncAudioTracksToTimeline) — earlier attempts derived the timeline
    // position FROM mpv's own position, which entangles the playhead with
    // whatever clip mapping was last locked in; that's what caused the
    // needle to chase clip drags, and then (after a snapshot-based patch)
    // caused playback to keep going stale after a clip moved away. Treating
    // wall-clock time as authoritative and re-deriving content live from it
    // avoids both classes of bug at the architecture level rather than
    // patching individual symptoms.
    //
    // NOTE: preview VIDEO uses PRIORITY-based layering, not true alpha
    // blending — on every sync, ALL video tracks are checked in reverse
    // index order (later/lower-in-the-list tracks first), and whichever is
    // the FIRST to have a clip covering the current position wins and is
    // what actually plays. A track with nothing at this position is
    // "transparent": the check falls through to the track above it. This
    // means a lower track acts as an overlay that shows through only where
    // it has content — a real, useful layering model, but clips don't
    // blend/fade into each other the way true compositing would; whichever
    // one wins fully replaces what's beneath it. Genuine alpha-blended
    // simultaneous video compositing would need a custom multi-source
    // decode+blend pipeline (abandoning mpv's native window rendering
    // entirely) — a separate, much larger undertaking than this.
    //
    // Independent multi-track audio MIXING (multiple audio tracks actually
    // summed together, not just each playing correctly on its own) is a
    // separate, much larger body of work — a real audio engine — and isn't
    // attempted here.
    void seekTimeline(double timelineSeconds);
    void syncVideoToTimeline(double timelineSeconds);
    void syncAudioTracksToTimeline(double timelineSeconds);
    // Composites whichever Overlay-track clip(s) cover the current position
    // via PlayerWidget::setOverlay — unlike video layering, overlays DO
    // stack simultaneously (each gets its own mpv overlay id), since
    // mpv's overlay-add genuinely supports multiple independent overlays
    // at once (this isn't priority-based like video).
    void syncOverlaysToTimeline(double timelineSeconds);

    // Produces the overlay bitmap for `clip` at clip-relative time `localSec`,
    // scaled and alpha-multiplied per its animation, reusing the cached bitmap
    // when nothing that affects pixels has changed. `outPos` receives the
    // top-left corner to composite at.
    QImage renderOverlayBitmap(int trackIndex, int clipIndex, const Clip& clip,
                               double localSec, const QSize& canvas, QPoint* outPos);

    int findClipIndexAt(const Track& track, double timelineSeconds) const;

    // --- Import helpers ----------------------------------------------------
    // Extracted from the toolbar Import flow so both it and drag-and-drop
    // from MediaBrowserPanel share the same clip-creation logic — the only
    // difference is where the resulting clip(s) land (end of track vs. the
    // exact drop position) and, for drag-and-drop, which track was dropped
    // onto.
    //
    // A video file gets a clip on the video track plus a companion clip on
    // Audio 1 (see Project.h's multi-track rationale); an audio-only file
    // (wav/mp3/etc) just gets one clip on whichever audio track it landed on.
    //
    // The companion audio clip is skipped when the source has no audio stream
    // (a GIF, a silent export) — MediaProbe::hasAudioStream decides, so the
    // test is what the file contains rather than what it's named.
    // --- Project file plumbing ---------------------------------------------
    // Rebuilds every piece of per-track machinery MainWindow owns to match
    // whatever is now in m_project — audio players, header panel, transcript
    // tabs, overlay bookkeeping. Loading replaces the project wholesale, so
    // none of that state can be assumed to still line up.
    void adoptLoadedProject(double playheadSec, double pixelsPerSecond);

    // Kicks off waveform and thumbnail generation for every clip. These are
    // deliberately not stored in the project file (see ProjectSerializer), so a
    // freshly opened project has clips with no visuals until this fills them in.
    void regenerateAllClipVisuals();

    // Marks the project as having unsaved changes and refreshes the title bar.
    // Cheap and idempotent, so it's safe to call from anything that mutates.
    void markProjectDirty();
    void updateWindowTitle();

    // Hides the GIFs dock when no Klipy API key is set. Called after the saved
    // layout is restored, since that can reinstate a dock the user can't use.
    void applyKlipyDockVisibility();
    void updateKlipyMenuHint();

    // Offers to save when something is about to discard unsaved work.
    // Returns false only if the user chose Cancel, meaning: don't proceed.
    bool confirmDiscardChanges();

    void setCurrentProjectPath(const QString& path);
    void rememberRecentProject(const QString& path);
    void rebuildRecentProjectsMenu();

    void importVideoFileAt(const QString& path, int videoTrackIndex, double trackPosSec);
    void importAudioOnlyFileAt(const QString& path, int trackIndex, double trackPosSec);
    // Places a still image as an Overlay-track clip. There's no
    // intrinsic "duration" for a still image, so it gets a fixed default
    // (kDefaultOverlayClipLenSec) — like any clip, its length can then be
    // adjusted afterward by dragging its trim handles.
    void importOverlayFileAt(const QString& path, int overlayTrackIndex, double trackPosSec);

    // Creates the AudioPlayer + AudioTrackPlayback bookkeeping for one audio
    // track — used both for the tracks seeded at startup and for any
    // audio track added later via onAddAudioTrackClicked, so a freshly
    // added track gets independent playback immediately, matching every
    // other audio track.
    void setupAudioPlayerForTrack(int trackIndex);

    // Locates the whisper GGML model file: checks a conventional path next
    // to the executable first, then a previously-remembered choice (via
    // QSettings), then prompts via a file picker if neither exists —
    // remembering that choice for next time. Returns an empty string if
    // the user cancels the picker.
    // allowPrompt=false never opens a file picker — auto-transcription must not
    // throw a dialog at someone who simply opened the app.
    QString resolveWhisperModelPath(bool allowPrompt = true);

    // --- Automatic transcription ------------------------------------------
    // Audio tracks transcribe themselves. A scan is scheduled (debounced)
    // whenever the clip set might have changed; it compares each audio track's
    // current source files against Track::transcriptSignature and queues only
    // what's actually out of date. Jobs run one at a time — whisper is heavy
    // enough that running several at once would just make them all slower.
    void scheduleTranscriptionScan();
    void scanForTranscriptionWork();
    void startNextTranscriptionJob();

    // Sorted, de-duplicated source files on a track, joined — see
    // Track::transcriptSignature.
    QString transcriptSignatureFor(const Track& track) const;

    // Formats seconds as fixed-width HH:MM:SS for the transcript display —
    // deliberately always shows hours/leading zeros (unlike Timeline's own
    // formatTickLabel, which is compact/context-dependent), since a
    // transcript reads more like a subtitle file's timestamp track.
    QString formatTranscriptTimestamp(double seconds) const;

    // Rebuilds the transcript tab strip from scratch (one tab per current
    // audio track) — called whenever the track LIST itself changes
    // (add/delete/rename an audio track), since tab count/labels need to
    // match. Preserves which track's tab was active across the rebuild.
    void rebuildTranscriptTabs();

    // Repopulates just ONE tab's list after (re)transcribing that track,
    // and switches to it so the fresh result is immediately visible.
    void refreshTranscriptTab(int trackIndex);

    // Fills a transcript list widget with a track's segments (shared by
    // rebuildTranscriptTabs and refreshTranscriptTab).
    void populateTranscriptList(QListWidget* list, int trackIndex);

    // Rebuilds every transcript tab's rows in place. Needed after any edit that
    // moves audio on the timeline, since the displayed times are ruler
    // positions rather than offsets within the source file.
    void refreshTranscriptTimestamps();

    // Seeks to the timeline position stored on a transcript row.
    void seekToTranscriptRow(QListWidget* list, int row);

    // Which audio track's tab is currently active, or -1 if there are no
    // tabs at all — used by the search functions, which operate on
    // whichever tab is currently showing.
    int currentTranscriptTrackIndex() const;
    QListWidget* currentTranscriptList() const;

    // Shows/focuses the transcript search bar — wired to Ctrl+F.
    void showTranscriptSearch();

    // Maps a timestamp from a transcript (relative to the WHOLE source
    // file whisper transcribed, not the edited timeline) to an actual
    // timeline position, by finding whichever clip on `trackIndex`
    // references `sourcePath` and covers `sourceTimeSec` within its own
    // trim window, then applying that clip's position/trim offset.
    // Returns -1 if no current clip covers that moment (e.g. it was
    // trimmed away since transcribing).
    double mapSourceTimeToTimelineSec(int trackIndex, const QString& sourcePath, double sourceTimeSec) const;

    // Combines the master volume slider with a track's own volumePercent
    // (from its right-click menu) into the single value actually applied
    // to that track's AudioPlayer — e.g. master 50% * track 50% = 25%.
    int combinedVolumeForTrack(int trackIndex) const;

    // Refreshes both track-view widgets after the track LIST itself
    // changes (add/delete/reorder) — as opposed to just a clip within an
    // existing track. Beyond the widgets' own setProject() calls (which
    // handle their own resize), this also nudges the scroll areas
    // themselves, since track-count changes are exactly the case where the
    // scrollbar range needs to grow to reach newly added tracks.
    void refreshTrackViews();

    // --- Playback drift correction ----------------------------------------
    // The timeline clock is wall time and each mpv instance keeps its own, so
    // they separate continuously. These govern how that gap is closed. See
    // syncVideoToTimeline for why a rate nudge is used in preference to a seek.

    // Below this, nothing is done. Without a dead zone the loop would chase
    // sub-frame error forever, changing rate on every tick.
    static constexpr double kDriftDeadZoneSec = 0.04;   // about one frame at 25fps

    // Above this the gap is too large to close by rate and indicates a real
    // desync -- a stall, a scrub, a clip change -- rather than accumulated
    // drift. Seek instead.
    //
    // Video and audio differ deliberately. A video seek flushes the decoder and
    // drops frames, so it is worth avoiding until the gap is large; an audio
    // seek is close to imperceptible. Audio also matters more when it is wrong,
    // because a quarter-second offset against picture is obvious lip-sync
    // error, while the same offset in video alone is invisible.
    static constexpr double kHardResyncSec = 1.5;
    static constexpr double kAudioHardResyncSec = 0.25;

    // Fraction of the remaining error corrected per tick. Chosen so the
    // steady-state error lands INSIDE the dead zone for a plausible clock error
    // -- too low and the loop settles at a permanent offset it never closes
    // (0.08 leaves about 150ms), too high and it hunts.
    static constexpr double kDriftGain = 0.4;

    // Rate-change ceilings, which bound how fast an error can be closed.
    // Video carries no audio on its player (aid=no), so a brisk correction is
    // invisible. Audio is audible: mpv corrects pitch when changing tempo, but
    // 1% is the point below which the change cannot be noticed at all.
    static constexpr double kMaxVideoNudge = 0.08; // 8%
    static constexpr double kMaxAudioNudge = 0.01; // 1%

    // How long to leave a seek alone before judging the result. Comfortably
    // longer than a keyframe seek takes, because the cost of waiting slightly
    // too long is one extra tick of drift, while the cost of not waiting long
    // enough is the seek storm this exists to prevent.
    static constexpr int kSeekSettleMs = 400;

    Project m_project;

    // Where this project lives on disk, empty for one that has never been
    // saved. Drives the title bar, and decides whether Save can write straight
    // out or has to ask for a location first.
    QString m_currentProjectPath;
    bool m_projectDirty = false;
    class QMenu* m_recentMenu = nullptr;
    PlayerWidget* m_player = nullptr;
    Timeline* m_timeline = nullptr;
    TrackHeaderPanel* m_trackHeaderPanel = nullptr;
    MediaBrowserPanel* m_mediaBrowser = nullptr;
    OverlayInspectorPanel* m_overlayInspector = nullptr;
    KlipyPanel* m_klipyPanel = nullptr;
    OverlayStageWidget* m_overlayStage = nullptr;
    QScrollArea* m_timelineScrollArea = nullptr;
    QScrollArea* m_headerScrollArea = nullptr;
    // One tab per audio track, each holding that track's own transcript —
    // see Track::transcript. m_transcriptListByTrack maps audio track
    // index -> its QListWidget (the tab's page), so results/search can
    // target the right one without scanning tabs by label.
    QTabWidget* m_transcriptTabs = nullptr;
    QHash<int, QListWidget*> m_transcriptListByTrack;
    QLineEdit* m_transcriptSearchBox = nullptr;
    QPushButton* m_chooseModelButton = nullptr;
    QVector<int> m_transcriptSearchMatches; // row indices into the ACTIVE tab's list matching the current search
    int m_transcriptSearchCurrentMatch = -1;
    // One queued unit of transcription work: one source file, destined for one
    // audio track's transcript.
    struct TranscriptionJob {
        int trackIndex = -1;
        QString sourcePath;
        bool firstForTrack = false; // clears the track's existing transcript before appending
    };
    QVector<TranscriptionJob> m_transcriptionQueue;
    bool m_transcriptionRunning = false;
    QTimer* m_transcriptScanTimer = nullptr;
    QProgressBar* m_transcribeProgressBar = nullptr;
    QLabel* m_transcribeStatusLabel = nullptr;
    QAction* m_splitAction = nullptr;
    QAction* m_pinAction = nullptr;
    // Left-hand status-bar readout: track count and project duration. Cheap to
    // keep current and it answers "how long is this thing" without measuring
    // against the ruler.
    QLabel* m_projectStatsLabel = nullptr;

    QDockWidget* m_playerDock = nullptr;
    QDockWidget* m_transcriptDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_mediaBrowserDock = nullptr;
    QDockWidget* m_overlayDock = nullptr;
    QDockWidget* m_klipyDock = nullptr;
    class QAction* m_klipyViewAction = nullptr;
    class SoundEffectsPanel* m_soundEffectsPanel = nullptr;
    QDockWidget* m_soundEffectsDock = nullptr;

    // Note: selection itself is owned entirely by Timeline (supports
    // multi-select) — MainWindow queries m_timeline->selectedClips() on
    // demand (e.g. for Split) rather than keeping a parallel copy.

    // Video-track playback state. m_playingVideoTrackIndex is needed
    // alongside m_playingClipIndex now that MULTIPLE video tracks exist —
    // a clip index alone is ambiguous across tracks (e.g. clip 0 could
    // exist on both Video 1 and Video 2).
    int m_playingVideoTrackIndex = -1;
    int m_playingClipIndex = -1;       // index into that track's clips currently loaded, or -1 (nothing/gap)
    QString m_currentLoadedPath;       // path of whatever file is actually loaded in the player right now
    bool m_awaitingSeekAfterLoad = false;
    double m_pendingSeekSec = 0.0;

    // Video's equivalent of the per-track seek guard above; see the note there.
    QElapsedTimer m_sinceVideoSeek;
    bool m_videoSeekInFlight = false;

    // Per-overlay-track state: which clip (if any) is currently composited
    // for that track, so setOverlay/clearOverlay are only called on an
    // actual change rather than redundantly every tick.
    QHash<int, int> m_activeOverlayClipByTrack; // trackIndex -> clipIndex, or absent/-1 if none showing

    // What was last actually pushed to mpv for an overlay track. Scaling and
    // alpha-blending an image is far too expensive to redo on every 16ms tick,
    // so the rendered bitmap is rebuilt only when the size or opacity actually
    // changes; pure position changes reuse the existing bitmap and just re-issue
    // overlay-add with new coordinates, which costs nothing.
    struct OverlayRenderCache {
        int clipIndex = -1;
        // Which frame of an animated source is currently rendered. Part of the
        // key, or an animation would composite frame zero forever: every other
        // field stays identical from one frame to the next, so without this the
        // cache reports a hit for the entire clip.
        int frameIndex = -1;
        int width = -1;
        int height = -1;
        int opacityStep = -1;  // opacity quantised to 0..255
        int rotationStep = 0;  // rotation quantised to whole degrees
        bool fastQuality = false; // rendered with fast scaling during a drag
        QImage rendered;
    };
    QHash<int, OverlayRenderCache> m_overlayCacheByTrack;

    // Clips (encoded as (trackIndex << 32) | clipIndex) currently
    // regenerating a higher-resolution thumbnail strip in the background —
    // see onThumbnailDetailNeeded. Prevents queuing duplicate jobs for the
    // same clip while one is already in flight.
    QSet<qint64> m_pendingThumbnailUpgrades;

    // The authoritative timeline position — see the architecture note above.
    double m_currentTimelineSec = 0.0;

    // Master playback intent — set only by the user's Play/Pause button
    // (PlayerWidget::userToggledPlayback). Drives whether the master clock
    // timer runs at all, and what play/pause state every player (video and
    // every audio track) should be brought to on each sync.
    bool m_isPlayingIntent = false;

    // Per-audio-track playback state.
    QVector<AudioTrackPlayback> m_audioTracks;

    // Wall-clock master timeline clock. Ticks only while m_isPlayingIntent
    // is true; each tick advances m_currentTimelineSec by real elapsed time
    // (via QElapsedTimer::restart(), so it's robust to non-exact 16ms timer
    // granularity) and re-syncs both video and every audio track to it.
    QTimer* m_masterClockTimer = nullptr;
    QElapsedTimer m_masterClockElapsed;

    // Current master volume (0-100), applied to every audio track's
    // AudioPlayer — remembered so newly-added audio tracks (see
    // setupAudioPlayerForTrack) start at the level the user already set,
    // rather than always defaulting back to 100.
    int m_masterVolumePercent = 100;

    // Zoom multiplier per click of the zoom in/out buttons.
    static constexpr double kZoomFactor = 1.25;
    // Default length for a freshly-imported overlay clip (stills have no
    // intrinsic duration) — adjustable afterward via trim handles.
    static constexpr double kDefaultOverlayClipLenSec = 5.0;
};
