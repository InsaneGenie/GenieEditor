#pragma once

#include <QWidget>
#include <QSet>
#include <QPair>
#include "Project.h"
#include "TimelineMetrics.h"

class QWheelEvent;
class QMouseEvent;
class QPaintEvent;
class QEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QKeyEvent;
class QContextMenuEvent;

// Renders the multi-track timeline: one lane per track, clips as rectangles
// sized by duration, markers as colored overlays on a ruler strip up top.
// Also owns direct-manipulation gestures — dragging a clip (or a whole
// multi-selection of clips) to move it, dragging an edge to trim, magnetic
// snapping to other clips/the playhead, and deleting selected clips —
// mutating the shared Project directly, since these gestures don't need a
// separate validation step the way Split does.
//
// Multi-selection: plain click selects just that clip; Ctrl+click toggles a
// clip's membership in the selection without affecting others; clicking an
// already-selected clip (no modifier) keeps the whole group selected so it
// can be dragged together. Delete/Backspace or the right-click menu act on
// the entire current selection.
//
// The MainWindow/controller still decides what higher-level actions (split,
// selection-driven operations) mean — it queries selectedClips() on demand
// rather than Timeline pushing every selection change — this widget just
// visualizes `Project` and reports/performs direct-manipulation gestures.
class Timeline : public QWidget {
    Q_OBJECT
public:
    explicit Timeline(QWidget* parent = nullptr);

    void setProject(Project* project);
    void setPlayheadSec(double seconds);
    void setPixelsPerSecond(double pxPerSec);
    double pixelsPerSecond() const { return m_pxPerSec; }

    // How far the enclosing scroll area has scrolled this widget vertically.
    //
    // The ruler and playhead needle are painted at this offset so they stay
    // pinned to the top of the viewport while the tracks scroll underneath —
    // the ruler is what every position on the timeline is read against, so
    // losing it off the top loses the thing that makes the rest legible.
    //
    // The widget can't work this out for itself: it's the scrolled CHILD, and
    // its own geometry says nothing about where the viewport currently sits.
    void setVerticalScrollOffset(int px);

    // Drops a pin at the current playhead position. If a pin already sits there
    // it's removed instead, so the same key both places and clears one — and it
    // stops repeated presses stacking invisible duplicates on the same frame.
    void togglePinAtPlayhead();

    // Computes and applies the zoom level that fits the entire project
    // duration into the given viewport width (e.g. the scroll area's
    // viewport). Falls back to a sane default if there's no project/content.
    void zoomToFit(int viewportWidthPx);

    // QScrollArea (which will host this widget) sizes its scroll range off
    // of sizeHint()/minimumSizeHint(), so this must reflect the actual
    // content width/height (project duration * zoom, and per-track heights)
    // rather than a fixed default.
    QSize sizeHint() const override;

    // Programmatically selects a single clip (e.g. to keep an external
    // controller's selection state in sync), replacing any existing
    // selection. Clicking a clip in the timeline already updates selection
    // internally and repaints immediately, before clipSelected even
    // reaches a listener.
    void setSelectedClip(int trackIndex, int clipIndex);

    // Every currently-selected (trackIndex, clipIndex) pair — queried
    // on-demand by MainWindow for batch operations (e.g. Split) rather than
    // tracked in a parallel copy.
    QVector<QPair<int, int>> selectedClips() const;

    // Clears the current selection entirely — used by MainWindow after a
    // track is deleted, since selection keys encode a track index that may
    // no longer point at the right track (or may not exist at all) once
    // tracks have shifted.
    void clearSelection();

signals:
    void seekRequested(double seconds);
    // Fired on every selection change with the most-recently-touched clip —
    // informational (e.g. for a future inspector panel); batch operations
    // should use selectedClips() instead of accumulating these.
    void clipSelected(int trackIndex, int clipIndex);
    void splitRequested(int trackIndex, int clipIndex, double timelineSec);
    void zoomChanged(double pxPerSec);
    // Fired right after a Ctrl+scroll zoom is applied. `anchorSec` is the
    // timeline second that was under the cursor, `oldPixelX` its pixel-space
    // x-coordinate BEFORE the zoom changed — MainWindow uses these to shift
    // the scroll area's horizontal scrollbar so that exact point stays
    // visually under the cursor after zooming, rather than the view
    // effectively zooming from the timeline's start.
    void zoomAnchorChanged(double anchorSec, int oldPixelX);
    // Fired on Shift+scroll — MainWindow applies this directly to the
    // scroll area's horizontal scrollbar (Timeline doesn't own it). Value
    // is a pixel delta in the natural scroll direction (positive = scroll
    // toward later content).
    void horizontalScrollRequested(int deltaPixels);
    // Fired when a file is dropped from an external source (e.g.
    // MediaBrowserPanel) onto the timeline — MainWindow performs the actual
    // import (probing, waveform/thumbnail generation, clip creation),
    // Timeline just reports where the drop landed.
    void mediaDropped(const QString& filePath, int trackIndex, double timelineSec);
    // Fired during paintEvent when a video clip's current thumbnail
    // resolution can't fill its on-screen width densely at the current
    // zoom — `desiredFullFileFrameCount` is how many thumbnails the WHOLE
    // source file should be regenerated at (not just this clip's trimmed
    // slice), matching the "generate once over the whole file, slice at
    // draw time" storage model. Safe to emit liberally — MainWindow
    // debounces (skips if already regenerating, or not a meaningful
    // improvement) rather than this side needing to rate-limit.
    void thumbnailDetailNeeded(int trackIndex, int clipIndex, int desiredFullFileFrameCount);
    // Fired after one or more clips have already been removed (via
    // Delete/Backspace or the right-click menu) — MainWindow uses this to
    // re-sync playback, since a removed clip might have been the one
    // currently loaded. Indices aren't meaningful for a multi-delete, so
    // this carries no specific clip identity.
    void clipDeleted();

    // Fired once, on release, when a move gesture ended with clips in a
    // DIFFERENT track than they started in. MainWindow re-syncs playback off
    // this for the same reason it does on delete: an audio clip that changed
    // track is now owned by a different AudioPlayer, and a video clip that
    // changed track may have changed which layer wins at the playhead.
    //
    // Deliberately not emitted per lane crossing during the drag — resyncing
    // reloads media into the players, and doing that on every lane the pointer
    // passes through would stutter the gesture for no benefit.
    void clipsMovedBetweenTracks();

    // Emitted whenever this widget has changed the Project in ANY way — a clip
    // moved, trimmed, split, deleted, or a pin added.
    //
    // Separate from the more specific signals above because it answers a
    // different question. Those say "this particular thing happened, go re-sync
    // playback"; this one only says "the project is no longer what was last
    // saved". Without it, unsaved-changes tracking would miss drags and trims
    // entirely, since those mutate Project directly and announce nothing.
    void projectModified();

    // A clip's playback rate changed. Distinct from projectModified because it
    // needs an immediate response rather than just a dirty flag: clip durations
    // just changed, and the players have to be told the new rate.
    void clipSpeedChanged();

    // Emitted after this widget has already added or removed a pin in the
    // Project — same "act directly, then announce" pattern the clip edits use.
    void markersChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class DragMode { None, MoveClip, TrimLeft, TrimRight };

    // One clip's starting position, snapshotted at drag-start, for group
    // moves — every selected clip gets one of these so the whole group can
    // be shifted by the same final (post-snap) delta together.
    //
    // The track is recorded TWICE because a move can now cross lanes.
    // `startTrackIndex` is the anchor the vertical offset is measured from and
    // never changes for the life of the drag; `trackIndex`/`clipIndex` are
    // where the clip currently lives and are rewritten every time the gesture
    // crosses into a new lane. Deriving the offset from the live index instead
    // would make it self-referential — each crossing would move the anchor it
    // was measured against, so the clip would walk away under the pointer.
    struct DragClipSnapshot {
        int startTrackIndex = -1;
        int trackIndex = -1;
        int clipIndex = -1;
        double startTrackPosSec = 0.0;
    };

    struct DragState {
        DragMode mode = DragMode::None;
        int primaryTrackIndex = -1; // the clip actually grabbed — used for trim, and as the snap reference
        int primaryClipIndex = -1;
        int primaryStartTrackIndex = -1; // the lane it was grabbed FROM; the vertical anchor
        // Which entry of movingClips is the primary. Identity by position in
        // the snapshot list rather than by (track, clip) pair, because that
        // pair is exactly what changes when the group crosses a lane.
        int primarySnapshotIndex = -1;
        // How many lanes OF THE CLIP'S OWN TYPE the group currently sits from
        // where it started. 0 for a purely horizontal move.
        int laneOffset = 0;
        QPoint startMousePos;
        double startSourceInSec = 0.0;  // trim only ever affects the primary clip
        double startSourceOutSec = 0.0;
        QVector<DragClipSnapshot> movingClips; // populated only for MoveClip mode
    };

    struct HitResult {
        int trackIndex = -1;
        int clipIndex = -1;
    };

    static qint64 clipKey(int trackIndex, int clipIndex);

    HitResult hitTest(const QPoint& pos) const;
    int trackIndexAtY(int y) const;

    // The clickable flag drawn in the ruler for a pin at `sec`.
    QRect pinHandleRect(double sec) const;

    // The ruler band's current top/bottom in WIDGET coordinates, which move
    // with the scroll offset. Every hit test that used to compare against
    // kRulerHeight has to go through these instead: at a scroll offset of 300,
    // widget y=310 is under the floating ruler even though it is nowhere near
    // the top of the widget.
    int rulerTopY() const { return m_scrollOffsetY; }
    int rulerBottomY() const { return m_scrollOffsetY + TimelineMetrics::kRulerHeight; }
    bool isRulerY(int y) const { return y >= rulerTopY() && y < rulerBottomY(); }
    // Index into Project::markers of the pin whose flag is under `pos`, or -1.
    int pinIndexAt(const QPoint& pos) const;
    double xToSec(int x) const;
    int secToX(double sec) const;
    DragMode dragModeAt(const QPoint& pos, int trackIndex, int clipIndex) const;
    QVector<double> collectSnapTargets(const QSet<qint64>& excludeKeys) const;

    // --- Vertical (cross-track) moves --------------------------------------
    // Lanes are counted WITHIN a type, not by raw track index, because tracks
    // are stored in creation order and types interleave: the default project is
    // Video 1 / Audio 1 / Audio 2, and adding a second video track appends it
    // after the audio ones. A raw index delta of +1 from Video 1 would land on
    // Audio 1, which is meaningless. Counting "the Nth video track" instead
    // makes dragging down from Video 1 go to Video 2 wherever it happens to sit
    // in the vector, and makes a mixed selection move each clip within its own
    // kind — the behaviour Premiere has and the one people expect.

    // How many tracks of the same type precede this one. -1 if out of range.
    int sameTypeOrdinal(int trackIndex) const;
    // The track index of the `ordinal`-th track of `type`, or -1 if there
    // isn't one — which is what stops a drag past the last lane.
    int trackForSameTypeOrdinal(TrackType type, int ordinal) const;
    // True only if EVERY clip in the current move has a destination lane at
    // this offset. All-or-nothing on purpose: letting part of a group move
    // while the rest stays put silently destroys the relative arrangement the
    // group was selected for.
    bool canMoveDraggedClipsToLane(int laneOffset) const;
    // Performs the relocation and repairs every index that depended on the old
    // positions. Safe to call with the current offset (does nothing).
    void moveDraggedClipsToLane(int laneOffset);

    // Applies a playback rate to every selected clip. Each clip keeps its own
    // start position — a clip's start is where the user put it, and only its
    // LENGTH follows from the rate.
    void applySpeedToSelection(double speed);
    void updateCursorForPosition(const QPoint& pos);
    void deleteClip(int trackIndex, int clipIndex);
    void deleteSelectedClips();

    // --- Clipboard ----------------------------------------------------------
    // Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D over the timeline. Internal rather than
    // the system clipboard: what is being copied is an edit decision -- a
    // reference to a source file with in/out points and a set of transforms --
    // which means nothing to any other application, and would be actively
    // wrong to paste into one.
    void copySelection();
    void cutSelection();
    void pasteAtPlayhead();
    void duplicateSelection();

    bool hasClipboardContent() const { return !m_clipboard.isEmpty(); }

    // One copied clip, described relative to the group rather than absolutely.
    //
    // Positions are stored as offsets from the EARLIEST clip in the selection,
    // and tracks as an offset within their own type, so a paste can land the
    // group at the playhead on whatever track is current while preserving both
    // the spacing between clips and which of them sat above which. Storing
    // absolute positions would paste everything back exactly where it was
    // copied from, which is never what is wanted.
    struct ClipboardEntry {
        Clip clip;
        TrackType trackType = TrackType::Video;
        int typeOrdinalOffset = 0; // relative to the topmost track in the copy
        double timeOffsetSec = 0.0; // relative to the earliest clip in the copy
    };
    QVector<ClipboardEntry> m_clipboard;

    Project* m_project = nullptr;
    double m_playheadSec = 0.0;
    double m_pxPerSec = 60.0; // zoom level
    int m_scrollOffsetY = 0;  // see setVerticalScrollOffset
    QSet<qint64> m_selectedClipKeys;

    DragState m_drag;
    QVector<double> m_snapTargets;   // candidate seconds to snap to, gathered at drag start
    double m_activeSnapSec = -1.0;   // >= 0 while a snap guide line should be drawn

    double m_hoverSec = -1.0;        // >= 0 while hovering the ruler (scrub preview)

    // Frame-preview popup state — a floating thumbnail shown above the
    // cursor while hovering a video clip's body, previewing the nearest
    // cached frame for that exact second (a scrub-preview, like hovering a
    // video player's seek bar). Cleared on drag start / mouse leave.
    int m_hoverPreviewTrackIndex = -1;
    int m_hoverPreviewClipIndex = -1;
    double m_hoverPreviewSec = -1.0;
    QPoint m_hoverPreviewMousePos;

    // Drag-hover preview state — a ghost bar shown while an external file
    // (e.g. from MediaBrowserPanel) is being dragged over the timeline, so
    // you can see where it'll land before releasing.
    bool m_isDragHoverActive = false;
    double m_dragPreviewDurationSec = 5.0; // probed once on dragEnter; fallback if probing fails
    double m_dragPreviewStartSec = 0.0;
    int m_dragPreviewTrackIndex = -1;
    QString m_dragPreviewFileName;

    // Floor is set low enough to fit ~900+ hours of content in a typical
    // window width — comfortably covers 50+ hour source material with
    // headroom, since visible duration at min zoom = viewportWidth / this.
    static constexpr double kMinPxPerSecond = 0.0005;
    static constexpr double kMaxPxPerSecond = 800.0;
    static constexpr int kTrimHandlePx = 6;
    // Pin flag geometry. The hit area is padded outward from the drawn shape so
    // a pin stays comfortably clickable without having to draw it oversized.
    static constexpr int kPinWidth = 13;
    static constexpr int kPinHeight = 15;
    static constexpr int kPinHitPadPx = 3;
    static constexpr double kMinClipLenSec = 0.05;
    static constexpr double kSnapThresholdPx = 8.0;
};
