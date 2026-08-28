#include "OverlayStageWidget.h"
#include "OverlayImageLoader.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QEvent>
#include <QImage>
#include <QtMath>
#include <algorithm>
#include <cmath>

OverlayStageWidget::OverlayStageWidget(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                          | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);

    // This window must never take keyboard focus. It's a manipulation surface,
    // not a place you type — and activating it would pull focus off the main
    // window, so pressing Delete or M straight after nudging an overlay would go
    // nowhere instead of reaching the timeline.
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);

    setMouseTracking(true);
    hide();
}

void OverlayStageWidget::attachTo(QWidget* surface) {
    if (m_surface) m_surface->removeEventFilter(this);
    m_surface = surface;
    if (!m_surface) return;

    // Watching the surface rather than polling: the stage has to follow it
    // through window moves, resizes, dock floating and tab switches, and every
    // one of those arrives as an event on the surface itself.
    m_surface->installEventFilter(this);
    if (m_surface->window()) m_surface->window()->installEventFilter(this);
    refresh();
}

bool OverlayStageWidget::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
        refresh();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

Clip* OverlayStageWidget::targetClip() const {
    if (!m_project) return nullptr;
    if (m_trackIndex < 0 || m_trackIndex >= m_project->tracks.size()) return nullptr;
    Track& track = m_project->tracks[m_trackIndex];
    if (track.type != TrackType::Overlay) return nullptr;
    if (m_clipIndex < 0 || m_clipIndex >= track.clips.size()) return nullptr;
    return &track.clips[m_clipIndex];
}

double OverlayStageWidget::localSec() const {
    const Clip* clip = targetClip();
    return clip ? m_playheadSec - clip->trackPosSec : 0.0;
}

bool OverlayStageWidget::canKeyframe() const {
    const Clip* clip = targetClip();
    if (!clip) return false;
    const double t = localSec();
    return t >= -0.001 && t <= clip->durationSec() + 0.001;
}

void OverlayStageWidget::setTarget(Project* project, int trackIndex, int clipIndex) {
    m_project = project;
    m_trackIndex = trackIndex;
    m_clipIndex = clipIndex;
    m_activeGrip = Grip::None;
    refresh();
}

void OverlayStageWidget::setPlayheadSec(double timelineSec) {
    m_playheadSec = timelineSec;
    // Skipped mid-drag: the pointer is the authority while a handle is held, and
    // letting playback rewrite the box underneath it would fight the user.
    if (m_activeGrip == Grip::None) refresh();
}

void OverlayStageWidget::refresh() {
    const Clip* clip = targetClip();
    const bool visible = clip && m_surface && m_surface->isVisible()
                      && m_surface->window() && m_surface->window()->isVisible();

    if (!visible) {
        if (isVisible()) hide();
        m_lastPaintedBounds = QRect();
        return;
    }

    // The clip has to actually be under the playhead — handles floating over a
    // frame where the overlay isn't showing would be pointing at nothing.
    const double t = localSec();
    if (t < 0.0 || t > clip->durationSec()) {
        if (isVisible()) hide();
        m_lastPaintedBounds = QRect();
        return;
    }

    // Only touch the window when something actually moved. setGeometry, show and
    // raise are each a real window-manager call, and doing all three on every
    // 16ms playback tick was making the whole stage stutter — raise() in
    // particular is expensive and causes visible flicker on a translucent
    // always-on-top window.
    const QRect wanted(m_surface->mapToGlobal(QPoint(0, 0)), m_surface->size());
    if (geometry() != wanted) setGeometry(wanted);
    if (!isVisible()) {
        show();
        raise(); // only on the transition into visibility, never per frame
    }

    // Repaint just the union of where the box was and where it is now, rather
    // than the entire video-sized translucent window. Alpha-composited repaints
    // are expensive per pixel, and the box occupies a small fraction of it.
    const QRect current = boxBoundsPx();
    const QRect dirty = current.united(m_lastPaintedBounds);
    m_lastPaintedBounds = current;
    if (dirty.isValid()) {
        update(dirty.adjusted(-4, -4, 4, 4));
    } else {
        update();
    }
}

QRect OverlayStageWidget::boxBoundsPx() const {
    if (!targetClip()) return QRect();

    // Union of the rotated corners plus the rotation arm, padded for the handle
    // squares and the stroke width.
    QRectF bounds;
    for (int i = 0; i < 4; ++i) {
        const QPointF corner = cornerPx(i);
        bounds = bounds.isNull() ? QRectF(corner, QSizeF(1, 1)) : bounds.united(QRectF(corner, QSizeF(1, 1)));
    }
    bounds = bounds.united(QRectF(rotateHandlePx(), QSizeF(1, 1)));
    return bounds.toRect().adjusted(-kHandlePx, -kHandlePx, kHandlePx, kHandlePx);
}

QPointF OverlayStageWidget::centrePx() const {
    const Clip* clip = targetClip();
    if (!clip) return QPointF();
    const double t = localSec();
    // Against the VIDEO rect, not the widget, so a handle sits where the export
    // will actually draw the overlay.
    const QRectF v = videoRectF();
    return QPointF(v.x() + v.width() * clip->anim.x.valueAt(t),
                   v.y() + v.height() * clip->anim.y.valueAt(t));
}

QSizeF OverlayStageWidget::halfExtentPx() const {
    const Clip* clip = targetClip();
    if (!clip) return QSizeF();

    const QImage source = OverlayImageLoader::load(clip->sourcePath);
    if (source.isNull() || source.width() <= 0) return QSizeF();

    const double t = localSec();
    const double w = videoRectF().width() * std::clamp(clip->anim.scale.valueAt(t), 0.005, 4.0);
    const double h = w * source.height() / source.width();
    return QSizeF(w / 2.0, h / 2.0);
}

double OverlayStageWidget::rotationDeg() const {
    const Clip* clip = targetClip();
    return clip ? clip->anim.rotation.valueAt(localSec()) : 0.0;
}

QPointF OverlayStageWidget::cornerPx(int index) const {
    const QSizeF half = halfExtentPx();
    const double signX = (index == 0 || index == 3) ? -1.0 : 1.0;
    const double signY = (index == 0 || index == 1) ? -1.0 : 1.0;

    const double rad = qDegreesToRadians(rotationDeg());
    const double localX = signX * half.width();
    const double localY = signY * half.height();

    return centrePx() + QPointF(localX * std::cos(rad) - localY * std::sin(rad),
                                localX * std::sin(rad) + localY * std::cos(rad));
}

QPointF OverlayStageWidget::rotateHandlePx() const {
    const QSizeF half = halfExtentPx();
    const double rad = qDegreesToRadians(rotationDeg());
    const double localY = -(half.height() + kRotateArmPx);
    return centrePx() + QPointF(-localY * std::sin(rad), localY * std::cos(rad));
}

QSizeF OverlayStageWidget::aabbHalfExtentPx() const {
    const QSizeF half = halfExtentPx();
    const double rad = qDegreesToRadians(rotationDeg());
    const double c = std::abs(std::cos(rad));
    const double s = std::abs(std::sin(rad));
    return QSizeF(half.width() * c + half.height() * s,
                  half.width() * s + half.height() * c);
}

QPointF OverlayStageWidget::applySnapping(const QPointF& proposedCentre) {
    m_snapGuideX = -1.0;
    m_snapGuideY = -1.0;

    const QSizeF half = aabbHalfExtentPx();
    QPointF result = proposedCentre;

    // Each candidate is (centre position that achieves it, guide line to draw).
    // Left/right edges are expressed as centre positions rather than edge
    // positions so the comparison is against one number, not two.
    // Snap targets are the VIDEO's edges and centre. Snapping to the widget's
    // edges would place an overlay against a black bar rather than against the
    // picture, which is never what centring an overlay is meant to mean.
    const QRectF v = videoRectF();
    const double candX[3] = { v.left() + half.width(), v.center().x(), v.right() - half.width() };
    const double guideX[3] = { v.left(), v.center().x(), v.right() };
    const double candY[3] = { v.top() + half.height(), v.center().y(), v.bottom() - half.height() };
    const double guideY[3] = { v.top(), v.center().y(), v.bottom() };

    double bestX = kSnapPx;
    for (int i = 0; i < 3; ++i) {
        const double d = std::abs(proposedCentre.x() - candX[i]);
        if (d < bestX) { bestX = d; result.setX(candX[i]); m_snapGuideX = guideX[i]; }
    }

    double bestY = kSnapPx;
    for (int i = 0; i < 3; ++i) {
        const double d = std::abs(proposedCentre.y() - candY[i]);
        if (d < bestY) { bestY = d; result.setY(candY[i]); m_snapGuideY = guideY[i]; }
    }

    return result;
}

OverlayStageWidget::Grip OverlayStageWidget::gripAt(const QPoint& pos) const {
    if (!targetClip()) return Grip::None;
    const QPointF p(pos);

    const double reach = kHandlePx + 4;
    if (QLineF(p, rotateHandlePx()).length() <= reach) return Grip::Rotate;

    static const Grip corners[4] = {Grip::ScaleTL, Grip::ScaleTR, Grip::ScaleBR, Grip::ScaleBL};
    for (int i = 0; i < 4; ++i) {
        if (QLineF(p, cornerPx(i)).length() <= reach) return corners[i];
    }

    // Inside test done in the box's OWN unrotated frame: rotate the pointer back
    // by the overlay's angle and it becomes a plain rectangle comparison, which
    // is both simpler and exact.
    const QSizeF half = halfExtentPx();
    const QPointF rel = p - centrePx();
    const double rad = qDegreesToRadians(-rotationDeg());
    const double lx = rel.x() * std::cos(rad) - rel.y() * std::sin(rad);
    const double ly = rel.x() * std::sin(rad) + rel.y() * std::cos(rad);
    if (std::abs(lx) <= half.width() && std::abs(ly) <= half.height()) return Grip::Move;

    return Grip::None;
}

void OverlayStageWidget::paintEvent(QPaintEvent*) {
    if (!targetClip()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPolygonF hitBox;
    for (int i = 0; i < 4; ++i) hitBox << cornerPx(i);

    // --- Hit-test floor -----------------------------------------------------
    // WA_TranslucentBackground makes this a layered window, and layered windows
    // are hit-tested PER PIXEL against the alpha channel: anywhere alpha is 0,
    // the cursor never enters this window at all and the event goes to whatever
    // is underneath — here, mpv's video window.
    //
    // The box interior used to be fully transparent, so moving over it produced
    // no events. Only brushing the outline (opaque pixels) got one move event
    // through, which set the hover state and painted the hover wash, which THEN
    // gave the interior alpha and made dragging work. That's why it felt like
    // you had to touch the edge of the box before you could grab it.
    //
    // An alpha of 4/255 is about 1.5% — invisible over video, but non-zero,
    // which is all the hit test asks for. Laid down over exactly the regions
    // gripAt() treats as interactive, so what's clickable and what's grabbable
    // are the same shape by construction.
    //
    // Everywhere else stays at alpha 0 on purpose, so clicks that miss the
    // overlay still pass through rather than being swallowed by an invisible
    // sheet covering the whole video.
    constexpr int kHitAlpha = 4;
    const double hitRadius = kHandlePx + 4; // must match gripAt()'s reach
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, kHitAlpha));
    p.drawPolygon(hitBox);
    for (int i = 0; i < 4; ++i) p.drawEllipse(cornerPx(i), hitRadius, hitRadius);
    p.drawEllipse(rotateHandlePx(), hitRadius, hitRadius);

    // The rotation arm too, so the run between the box and the handle isn't a
    // dead gap that swallows a near-miss.
    p.setPen(QPen(QColor(255, 255, 255, kHitAlpha), hitRadius));
    p.drawLine((cornerPx(0) + cornerPx(1)) / 2.0, rotateHandlePx());
    p.setPen(Qt::NoPen);

    // Snap guides, drawn on top of the floor. Full-height / full-width lines
    // make it unambiguous WHICH edge you've landed on.
    if (m_snapGuideX >= 0.0) {
        p.setPen(QPen(QColor(0, 0, 0, 130), 3));
        p.drawLine(QPointF(m_snapGuideX, 0), QPointF(m_snapGuideX, height()));
        p.setPen(QPen(Theme::accent(), 1.2, Qt::DashLine));
        p.drawLine(QPointF(m_snapGuideX, 0), QPointF(m_snapGuideX, height()));
    }
    if (m_snapGuideY >= 0.0) {
        p.setPen(QPen(QColor(0, 0, 0, 130), 3));
        p.drawLine(QPointF(0, m_snapGuideY), QPointF(width(), m_snapGuideY));
        p.setPen(QPen(Theme::accent(), 1.2, Qt::DashLine));
        p.drawLine(QPointF(0, m_snapGuideY), QPointF(width(), m_snapGuideY));
    }

    // Outline, drawn as a rotated rect through the four computed corners so it
    // hugs the image at any angle.
    const QPolygonF& box = hitBox;

    // A barely-there fill while the pointer is over the body. The whole interior
    // has always been draggable, but nothing said so — there used to be a cross
    // drawn at the centre, which if anything implied the opposite: that the
    // centre was the one place you could grab.
    if (m_hoverGrip == Grip::Move || m_activeGrip == Grip::Move) {
        QColor wash = Theme::accent();
        wash.setAlpha(38);
        p.setPen(Qt::NoPen);
        p.setBrush(wash);
        p.drawPolygon(box);
    }

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 140), 3));
    p.drawPolygon(box);
    p.setPen(QPen(Theme::accent(), 1.4));
    p.drawPolygon(box);

    // Arm out to the rotation handle.
    const QPointF rotHandle = rotateHandlePx();
    const QPointF topMid = (cornerPx(0) + cornerPx(1)) / 2.0;
    p.setPen(QPen(QColor(0, 0, 0, 140), 3));
    p.drawLine(topMid, rotHandle);
    p.setPen(QPen(Theme::accent(), 1.4));
    p.drawLine(topMid, rotHandle);

    // Handles read as accent-filled chips with a dark rim rather than plain
    // white squares — the rim is what keeps them visible against a bright frame,
    // and the accent fill ties them to every other interactive cue in the app.
    auto drawHandle = [&](const QPointF& at, bool round, bool hot) {
        const double r = kHandlePx / 2.0 + (hot ? 2.0 : 0.0);
        p.setPen(QPen(QColor(0, 0, 0, 170), 1.4));
        p.setBrush(hot ? QColor(0xFF, 0xFF, 0xFF) : Theme::accent());
        if (round) p.drawEllipse(at, r, r);
        else p.drawRoundedRect(QRectF(at.x() - r, at.y() - r, r * 2, r * 2), 2, 2);
    };

    static const Grip corners[4] = {Grip::ScaleTL, Grip::ScaleTR, Grip::ScaleBR, Grip::ScaleBL};
    for (int i = 0; i < 4; ++i) {
        drawHandle(cornerPx(i), false, m_hoverGrip == corners[i] || m_activeGrip == corners[i]);
    }
    drawHandle(rotHandle, true, m_hoverGrip == Grip::Rotate || m_activeGrip == Grip::Rotate);

    const Clip* clip = targetClip();

    // Live numeric readout while dragging. Direct manipulation is fast but
    // imprecise by nature; showing the values as they change means you can drag
    // roughly into place and still see exactly where you landed, without
    // crossing the window to read the inspector.
    if (clip && m_activeGrip != Grip::None) {
        const double t = localSec();
        QString readout;
        switch (m_activeGrip) {
        case Grip::Rotate:
            readout = QString("%1\u00B0").arg(qRound(clip->anim.rotation.valueAt(t)));
            break;
        case Grip::Move:
            readout = QString("X %1%   Y %2%")
                          .arg(qRound(clip->anim.x.valueAt(t) * 100))
                          .arg(qRound(clip->anim.y.valueAt(t) * 100));
            break;
        default:
            readout = QString("Size %1%").arg(qRound(clip->anim.scale.valueAt(t) * 100));
            break;
        }

        p.setFont(Theme::monoFont(-1, QFont::DemiBold));
        const QFontMetrics fm(p.font());
        const int chipW = fm.horizontalAdvance(readout) + 20;
        const QSizeF half = aabbHalfExtentPx();

        // Sits below the box, or above it when that would fall off the bottom.
        int chipY = int(centrePx().y() + half.height() + 10);
        if (chipY + 26 > height()) chipY = int(centrePx().y() - half.height() - 36);
        const QRect chip(std::clamp(int(centrePx().x()) - chipW / 2, 4, std::max(4, width() - chipW - 4)),
                         std::clamp(chipY, 4, std::max(4, height() - 30)), chipW, 24);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 190));
        p.drawRoundedRect(chip, 12, 12);
        p.setPen(Theme::accent());
        p.drawText(chip, Qt::AlignCenter, readout);
    }

    // Warn when edits can't be recorded, rather than silently swallowing drags.
    if (clip && clip->anim.hasAnyKeys() && !canKeyframe()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 190));
        const QRect note(8, 8, 268, 24);
        p.drawRoundedRect(note, 12, 12);
        p.setPen(Theme::now());
        p.setFont(Theme::uiFont(-1, QFont::DemiBold));
        p.drawText(note, Qt::AlignCenter, "Playhead is outside this clip");
    }
}

void OverlayStageWidget::mousePressEvent(QMouseEvent* event) {
    Clip* clip = targetClip();
    if (!clip) { event->ignore(); return; }

    const Grip grip = gripAt(event->pos());
    if (grip == Grip::None) {
        // Nothing of ours was hit. Ignoring rather than accepting matters: this
        // window covers the whole video, and swallowing every stray click would
        // make the preview feel dead.
        event->ignore();
        return;
    }

    const double t = localSec();
    m_activeGrip = grip;
    m_dragStartPos = event->pos();
    m_dragStartX = clip->anim.x.valueAt(t);
    m_dragStartY = clip->anim.y.valueAt(t);
    m_dragStartScale = clip->anim.scale.valueAt(t);
    m_dragStartRotation = clip->anim.rotation.valueAt(t);

    const QPointF rel = QPointF(event->pos()) - centrePx();
    m_dragStartAngle = std::atan2(rel.y(), rel.x());
    m_dragStartRadius = std::max(4.0, std::hypot(rel.x(), rel.y()));
    event->accept();
}

void OverlayStageWidget::mouseMoveEvent(QMouseEvent* event) {
    Clip* clip = targetClip();
    if (!clip) return;

    if (m_activeGrip == Grip::None) {
        const Grip hover = gripAt(event->pos());
        if (hover != m_hoverGrip) {
            m_hoverGrip = hover;
            switch (hover) {
            case Grip::Move:   setCursor(Qt::SizeAllCursor); break;
            case Grip::Rotate: setCursor(Qt::CrossCursor); break;
            case Grip::None:   setCursor(Qt::ArrowCursor); break;
            default:           setCursor(Qt::SizeFDiagCursor); break;
            }
            update(boxBoundsPx().adjusted(-4, -4, 4, 4));
        }
        return;
    }

    const double t = localSec();
    const bool key = canKeyframe();
    const QPointF delta = QPointF(event->pos()) - m_dragStartPos;

    switch (m_activeGrip) {
    case Grip::Move: {
        // Against the video rect, matching centrePx(): the drag start has to be
        // converted from a normalised value the same way the handle was drawn,
        // or the box jumps to a different place the moment you grab it.
        const QRectF vr = videoRectF();
        QPointF centre(vr.x() + m_dragStartX * vr.width() + delta.x(),
                       vr.y() + m_dragStartY * vr.height() + delta.y());

        // Alt bypasses snapping, the usual escape hatch for placing something
        // deliberately just off an edge without fighting the magnet.
        if (!(event->modifiers() & Qt::AltModifier)) {
            centre = applySnapping(centre);
        } else {
            m_snapGuideX = -1.0;
            m_snapGuideY = -1.0;
        }

        // Normalised before storing, so a drag means the same fraction of the
        // frame no matter how large the preview panel happens to be.
        // And back out again against the same rect, so what is stored is a
        // fraction OF THE VIDEO -- which is exactly what the export reads.
        OverlayAnimation::applyValue(
            clip->anim.x, (centre.x() - vr.x()) / std::max(1.0, vr.width()), t, key);
        OverlayAnimation::applyValue(
            clip->anim.y, (centre.y() - vr.y()) / std::max(1.0, vr.height()), t, key);
        break;
    }
    case Grip::ScaleTL:
    case Grip::ScaleTR:
    case Grip::ScaleBL:
    case Grip::ScaleBR: {
        // Scaling by the pointer's DISTANCE from the centre rather than by its
        // x/y delta. That's what keeps a corner drag behaving sensibly once the
        // overlay is rotated — the box's own axes no longer line up with the
        // screen's, but distance from the centre is rotation-independent.
        const QPointF rel = QPointF(event->pos()) - centrePx();
        const double radius = std::hypot(rel.x(), rel.y());
        const double factor = radius / m_dragStartRadius;
        OverlayAnimation::applyValue(clip->anim.scale,
                                     std::clamp(m_dragStartScale * factor, 0.01, 3.0), t, key);
        break;
    }
    case Grip::Rotate: {
        const QPointF rel = QPointF(event->pos()) - centrePx();
        const double angle = std::atan2(rel.y(), rel.x());
        double degrees = m_dragStartRotation + qRadiansToDegrees(angle - m_dragStartAngle);

        // Shift snaps to 15°, the usual convention and genuinely useful for
        // getting something back to level.
        if (event->modifiers() & Qt::ShiftModifier) {
            degrees = std::round(degrees / 15.0) * 15.0;
        }
        OverlayAnimation::applyValue(clip->anim.rotation, degrees, t, key);
        break;
    }
    case Grip::None:
        break;
    }

    // Snap guides run the full width/height of the frame, so the scoped repaint
    // that's right for the box alone would leave them smeared behind. Whenever a
    // guide is showing — or was showing on the previous move — repaint whole.
    // The readout chip is drawn outside the box, so while dragging the scoped
    // repaint would clip it. Simplest correct answer: full repaints during a
    // drag, which is also when the box is moving the most anyway.
    const bool guidesInvolved = m_snapGuideX >= 0.0 || m_snapGuideY >= 0.0
                             || m_guidesWereVisible || m_activeGrip != Grip::None;
    m_guidesWereVisible = (m_snapGuideX >= 0.0 || m_snapGuideY >= 0.0);

    const QRect dirty = boxBoundsPx().united(m_lastPaintedBounds);
    m_lastPaintedBounds = boxBoundsPx();
    if (guidesInvolved) update();
    else update(dirty.adjusted(-4, -4, 4, 4));

    emit transformChanged();
}

void OverlayStageWidget::mouseReleaseEvent(QMouseEvent*) {
    if (m_activeGrip == Grip::None) return;
    m_activeGrip = Grip::None;
    m_snapGuideX = -1.0;
    m_snapGuideY = -1.0;
    m_guidesWereVisible = false;
    update(); // guides span the whole frame, so this one repaint isn't scoped
    update(boxBoundsPx().adjusted(-4, -4, 4, 4));
    // One final notification after the drag ends. MainWindow uses the drag state
    // to composite at reduced quality while the pointer is down, so this is what
    // triggers the full-quality redraw once it's up.
    emit transformChanged();
}


void OverlayStageWidget::setVideoRect(const QRect& rectInSurface) {
    if (m_videoRect == rectInSurface) return;
    m_videoRect = rectInSurface;
    update(); // the handles move even though nothing about the clip changed
}

QRectF OverlayStageWidget::videoRectF() const {
    // Falls back to the whole widget when the video rect is unknown -- before
    // the first frame loads there is nothing better to measure against, and the
    // handles are hidden at that point anyway.
    if (m_videoRect.isEmpty()) return QRectF(rect());
    return QRectF(m_videoRect);
}
