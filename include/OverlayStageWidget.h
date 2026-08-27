#pragma once

#include <QWidget>
#include <QPointF>
#include "Project.h"

// Direct manipulation of an overlay clip on top of the video preview: drag to
// move, corner handles to resize, a handle above the top edge to rotate.
//
// --- Why this is a separate top-level window --------------------------------
//
// PlayerWidget hands mpv a native window handle, and mpv then owns that window
// completely: it creates its own child window inside it, paints over anything
// Qt parents there, and receives the mouse input that lands on it. So a normal
// child widget cannot be used for this — it would be both invisible and deaf.
//
// This is therefore a frameless, translucent, always-on-top TOOL WINDOW that
// tracks the video surface's screen geometry. Being a real window of its own,
// it sits above mpv's child window and gets its own input.
//
// The honest alternative is switching PlayerWidget from `wid` embedding to
// mpv's render API and drawing into a QOpenGLWidget, after which ordinary Qt
// children would compose over the video and none of this would be necessary.
// That's the better long-term answer and the right fix if this ever proves
// fragile; it's also a rewrite of the working playback path, which is why it
// isn't what's done here.
class OverlayStageWidget : public QWidget {
    Q_OBJECT
public:
    explicit OverlayStageWidget(QWidget* parent = nullptr);

    // Begins tracking `surface`'s position and size. The stage follows it as the
    // window moves, resizes, or the dock is floated, and hides itself whenever
    // the surface is hidden.
    void attachTo(QWidget* surface);

    // Points the stage at a clip. Anything that isn't an overlay clip hides it,
    // so the handles never appear over footage they can't affect.
    void setTarget(Project* project, int trackIndex, int clipIndex);
    void setPlayheadSec(double timelineSec);

    // Recomputes the on-screen box from the clip's current animated values.
    // Called whenever the animation changes underneath us — from the inspector,
    // or from playback moving the playhead.
    void refresh();

    // True while a handle is held. MainWindow drops the overlay compositing to
    // fast scaling during a drag, which is the difference between a drag that
    // tracks the pointer and one that lags behind it.
    bool isDragging() const { return m_activeGrip != Grip::None; }

signals:
    // The clip's animation was edited by dragging. Emitted continuously during a
    // drag so the preview follows the pointer rather than snapping at the end.
    void transformChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Grip { None, Move, ScaleTL, ScaleTR, ScaleBL, ScaleBR, Rotate };

    Clip* targetClip() const;
    double localSec() const;
    bool canKeyframe() const;

    // Centre of the overlay in this widget's pixels, and its half-extents before
    // rotation. Everything else is derived from these three.
    QPointF centrePx() const;
    QSizeF halfExtentPx() const;
    double rotationDeg() const;

    // Corner positions WITH rotation applied — the handles have to sit on the
    // rotated box, otherwise they'd drift off the image the moment it turns.
    QPointF cornerPx(int index) const;   // 0=TL 1=TR 2=BR 3=BL
    QPointF rotateHandlePx() const;

    // Screen-space bounds of everything this widget draws, used to repaint only
    // the affected region instead of the whole video-sized translucent window.
    QRect boxBoundsPx() const;

    // Half-extents of the ROTATED box's axis-aligned bounding box. Edge snapping
    // works off this rather than the unrotated size, because what should line up
    // with the frame edge is what you can actually see touching it.
    QSizeF aabbHalfExtentPx() const;

    // Nudges a proposed centre onto a frame edge or the frame centre when it
    // lands close enough. Returns the adjusted position and records which guides
    // were hit so paintEvent can show them.
    QPointF applySnapping(const QPointF& proposedCentre);

    Grip gripAt(const QPoint& pos) const;

    Project* m_project = nullptr;
    int m_trackIndex = -1;
    int m_clipIndex = -1;
    double m_playheadSec = 0.0;

    QWidget* m_surface = nullptr;

    // Where the box was last painted, so a move repaints both the old and new
    // positions and doesn't leave the previous outline behind.
    QRect m_lastPaintedBounds;

    Grip m_activeGrip = Grip::None;
    Grip m_hoverGrip = Grip::None;
    QPointF m_dragStartPos;
    double m_dragStartX = 0.0;
    double m_dragStartY = 0.0;
    double m_dragStartScale = 0.0;
    double m_dragStartRotation = 0.0;
    double m_dragStartAngle = 0.0;   // pointer angle about the centre, at press
    double m_dragStartRadius = 1.0;  // pointer distance from centre, at press

    // Which snap guides the current drag is sitting on, in this widget's pixels.
    // -1 means "not snapped on that axis".
    double m_snapGuideX = -1.0;
    double m_snapGuideY = -1.0;
    bool m_guidesWereVisible = false;

    static constexpr int kHandlePx = 9;
    static constexpr int kRotateArmPx = 26;
    // Snap strength in screen pixels — deliberately in pixels rather than a
    // fraction of the frame, so it feels the same regardless of preview size.
    static constexpr double kSnapPx = 9.0;
};
