#include "Timeline.h"
#include "TimelineMetrics.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFontMetrics>
#include <QPainterPath>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QFileInfo>
#include <QHash>
#include "MediaProbe.h"
#include <algorithm>
#include <iterator>
#include <cmath>

using namespace TimelineMetrics;

qint64 Timeline::clipKey(int trackIndex, int clipIndex) {
    return (static_cast<qint64>(trackIndex) << 32) | static_cast<quint32>(clipIndex);
}

Timeline::Timeline(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(200);
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus); // needed to actually receive keyPressEvent (Delete/Backspace)
}

void Timeline::setProject(Project* project) {
    m_project = project;
    updateGeometry(); // content size may have changed — let the scroll area know
    // setMinimumHeight is a much stronger signal than sizeHint()/resize()
    // alone — it's a hard constraint on minimumSizeHint() that a QScrollArea
    // MUST respect (never shrinking the widget below it), regardless of
    // widgetResizable or any auto-resize timing quirks. Height only, since
    // width already correctly grows/shrinks via the widgetResizable(true)
    // stretch-to-fill-viewport behavior — this is specifically about
    // guaranteeing the scrollbar's range reflects newly added tracks.
    setMinimumHeight(sizeHint().height());
    resize(sizeHint());
    update();
}

void Timeline::setPlayheadSec(double seconds) {
    m_playheadSec = seconds;
    update();
}

void Timeline::setPixelsPerSecond(double pxPerSec) {
    m_pxPerSec = qBound(kMinPxPerSecond, pxPerSec, kMaxPxPerSecond);
    updateGeometry(); // width changes with zoom
    // Same reasoning as setProject()'s setMinimumHeight(): updateGeometry()
    // alone only schedules a layout *request* and isn't a reliable enough
    // signal on its own — setMinimumWidth() is a hard floor on
    // minimumSizeHint() that the scroll area is guaranteed to respect,
    // which is what actually makes the horizontal scrollbar appear once
    // zoomed content exceeds the viewport. Recomputed fresh on every zoom
    // change, so it naturally shrinks back down when zooming back out —
    // doesn't fight the widgetResizable(true) stretch-to-fill-viewport
    // behavior for narrower-than-viewport content, since a minimum is a
    // floor, not a fixed size.
    setMinimumWidth(sizeHint().width());
    resize(sizeHint());
    update();
    emit zoomChanged(m_pxPerSec);
}

void Timeline::zoomToFit(int viewportWidthPx) {
    const double durationSec = m_project ? m_project->durationSec() : 0.0;
    if (durationSec <= 0.0 || viewportWidthPx <= 0) {
        setPixelsPerSecond(60.0); // nothing to fit — fall back to the default zoom
        return;
    }
    // Small margin so the last clip isn't drawn flush against the edge.
    const double target = (viewportWidthPx * 0.97) / durationSec;
    setPixelsPerSecond(target);
}

QSize Timeline::sizeHint() const {
    const double durationSec = m_project ? m_project->durationSec() : 0.0;
    // Pad with a bit of trailing space so the last clip isn't flush against
    // the scroll area's edge, and leave room for at least one full track lane.
    const int w = std::max(600, secToX(durationSec) + 200);
    const int h = kRulerHeight + (m_project ? totalTracksHeight(*m_project) : kTrackHeight);
    return QSize(w, h);
}

int Timeline::secToX(double sec) const {
    return static_cast<int>(sec * m_pxPerSec);
}

double Timeline::xToSec(int x) const {
    return static_cast<double>(x) / m_pxPerSec;
}

QVector<QPair<int, int>> Timeline::selectedClips() const {
    QVector<QPair<int, int>> result;
    result.reserve(m_selectedClipKeys.size());
    for (qint64 key : m_selectedClipKeys) {
        result.push_back({static_cast<int>(key >> 32), static_cast<int>(key & 0xffffffffLL)});
    }
    return result;
}

void Timeline::clearSelection() {
    if (m_selectedClipKeys.isEmpty()) return;
    m_selectedClipKeys.clear();
    update();
}

namespace {
// Picks a "nice" tick spacing (in seconds) so labels never crowd together,
// regardless of zoom level. Steps up through human-friendly intervals —
// seconds, then minutes, then hours — rather than an arbitrary multiplier,
// so ticks land on round numbers (5s, 30s, 1m, 5m, 1h, ...) like a real NLE.
double niceTickIntervalSeconds(double pxPerSecond, double minPixelSpacing) {
    static const double niceValuesSec[] = {
        1, 2, 5, 10, 15, 30,                        // seconds
        60, 120, 300, 600, 900, 1800,               // 1m, 2m, 5m, 10m, 15m, 30m
        3600, 7200, 14400, 21600, 43200,            // 1h, 2h, 4h, 6h, 12h
        86400, 172800, 432000, 864000, 2592000      // 1d, 2d, 5d, 10d, 30d
    };
    for (double v : niceValuesSec) {
        if (v * pxPerSecond >= minPixelSpacing) return v;
    }
    return niceValuesSec[std::size(niceValuesSec) - 1];
}

// Formats a tick/hover label appropriately for its scale: "12s" while zoomed
// in, "3:45" once we're past a minute, "1:02:03" past an hour.
QString formatTickLabel(double totalSeconds) {
    const int totalSecs = static_cast<int>(totalSeconds + 0.5);
    const int hours = totalSecs / 3600;
    const int minutes = (totalSecs % 3600) / 60;
    const int secs = totalSecs % 60;

    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    if (minutes > 0) {
        return QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0'));
    }
    return QString("%1s").arg(secs);
}

// The playhead capsule's readout. Unlike formatTickLabel (compact and
// scale-dependent), this is deliberately FIXED-WIDTH within a given magnitude
// and always carries a tenths digit — it updates ~60 times a second during
// playback, and a label that changed width as the numbers ticked over would
// visibly jitter against the needle it's attached to.
QString formatPlayheadTimecode(double totalSeconds) {
    if (totalSeconds < 0) totalSeconds = 0;
    const int tenths = static_cast<int>(totalSeconds * 10) % 10;
    const int totalSecs = static_cast<int>(totalSeconds);
    const int hours = totalSecs / 3600;
    const int minutes = (totalSecs % 3600) / 60;
    const int secs = totalSecs % 60;

    if (hours > 0) {
        return QString("%1:%2:%3.%4")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'))
            .arg(tenths);
    }
    return QString("%1:%2.%3")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'))
        .arg(tenths);
}

// Draws a clip's audio waveform inside its body rect. `clip.waveformPeaks`
// covers the clip's ENTIRE original source file, so this maps the clip's
// trimmed [sourceInSec, sourceOutSec] window onto the matching slice of the
// peaks array before drawing — that's what makes waveforms stay correct
// after a clip has been split or re-trimmed without regenerating anything.
void drawWaveform(QPainter& p, const QRect& bodyRect, const Clip& clip, const QColor& tint) {
    if (clip.waveformPeaks.isEmpty() || clip.waveformSourceDurationSec <= 0.0) {
        return;
    }

    const int totalPeaks = clip.waveformPeaks.size();
    const double fraction = 1.0 / clip.waveformSourceDurationSec;
    int startIdx = static_cast<int>(clip.sourceInSec * fraction * totalPeaks);
    int endIdx = static_cast<int>(clip.sourceOutSec * fraction * totalPeaks);
    startIdx = std::clamp(startIdx, 0, totalPeaks - 1);
    endIdx = std::clamp(endIdx, startIdx + 1, totalPeaks);
    const int sliceLen = endIdx - startIdx;
    if (sliceLen <= 0 || bodyRect.width() <= 0) return;

    // Waveform sits ON the clip body in a light tint of the clip's own hue
    // rather than a fixed mint green, so a hue-rotated "Audio 3" reads as a
    // coherent object instead of a green waveform stuck on a teal clip.
    QColor waveColor = tint.lighter(165);
    waveColor.setAlpha(215);
    p.setPen(waveColor);
    const int midY = bodyRect.center().y();
    const int maxBarHeight = bodyRect.height() / 2 - 2;

    for (int x = 0; x < bodyRect.width(); ++x) {
        const int peakIdx = startIdx + (x * sliceLen) / bodyRect.width();
        const float peak = clip.waveformPeaks[std::clamp(peakIdx, startIdx, endIdx - 1)];
        const int barHalfHeight = static_cast<int>(peak * maxBarHeight);
        if (barHalfHeight <= 0) continue;
        const int screenX = bodyRect.left() + x;
        p.drawLine(screenX, midY - barHalfHeight, screenX, midY + barHalfHeight);
    }
}

// Draws a clip's video filmstrip inside its body rect, using the same
// "generate once over the whole file, re-slice on trim" pattern as the
// waveform above.
void drawThumbnails(QPainter& p, const QRect& bodyRect, const Clip& clip) {
    if (clip.thumbnails.isEmpty() || clip.thumbnailSourceDurationSec <= 0.0 || bodyRect.width() <= 0) {
        return;
    }

    const int totalThumbs = clip.thumbnails.size();
    const double fraction = 1.0 / clip.thumbnailSourceDurationSec;
    int startIdx = static_cast<int>(clip.sourceInSec * fraction * totalThumbs);
    int endIdx = static_cast<int>(clip.sourceOutSec * fraction * totalThumbs);
    startIdx = std::clamp(startIdx, 0, totalThumbs - 1);
    endIdx = std::clamp(endIdx, startIdx + 1, totalThumbs);
    const int count = endIdx - startIdx;
    if (count <= 0) return;

    const int cellWidth = std::max(1, bodyRect.width() / count);
    for (int i = 0; i < count; ++i) {
        const int cellX = bodyRect.left() + i * cellWidth;
        const int w = (i == count - 1) ? (bodyRect.right() - cellX + 1) : cellWidth;
        const QRect cell(cellX, bodyRect.top(), w, bodyRect.height());
        p.drawImage(cell, clip.thumbnails[startIdx + i]);
    }
}

// A clip's full colour set, derived from the single track identity hue so the
// header strip, body gradient and waveform can never drift out of relation to
// each other.
struct ClipPalette {
    QColor accent;      // the track's identity hue
    QColor headerColor; // darker name-strip along the top
    QColor bodyTop;     // body gradient, lit from above
    QColor bodyBottom;
};

ClipPalette paletteFor(TrackType type, int colorIndex) {
    const QColor accent = trackAccent(type, colorIndex);
    ClipPalette p;
    p.accent = accent;
    p.headerColor = accent.darker(175);
    p.bodyTop = accent.lighter(112);
    p.bodyBottom = accent.darker(128);
    return p;
}

// Rounded-rect helper on a QRectF, so clip edges stay crisp under
// antialiasing instead of landing on half-pixel boundaries.
QPainterPath roundedPath(const QRect& r, double radius) {
    QPainterPath path;
    path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    return path;
}

// The floating pill used for the ruler's hover time and the playhead readout.
// One helper for both means they're guaranteed to look like the same object.
void drawTimePill(QPainter& p, const QRect& rect, const QString& label,
                  const QColor& fill, const QColor& textColor, double radius) {
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect), radius, radius);
    p.setPen(textColor);
    p.setFont(Theme::monoFont(-1, QFont::DemiBold));
    p.drawText(rect, Qt::AlignCenter, label);
}
} // namespace

void Timeline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(rect(), Theme::bg0());

    if (!m_project) {
        p.fillRect(0, 0, width(), kRulerHeight, Theme::bg2());
        return;
    }

    const double tickIntervalSec = niceTickIntervalSeconds(m_pxPerSec, /*minPixelSpacing=*/78.0);
    const double minorIntervalSec = tickIntervalSec / 4.0;

    // -------------------------------------------------------------------
    // Track lanes
    // -------------------------------------------------------------------
    // Drawn before the ruler so the ruler's bottom border reads as the seam
    // between the two, and before the grid so the grid sits on top of the lane
    // wash rather than under it.
    int y = kRulerHeight;
    for (int trackIndex = 0; trackIndex < m_project->tracks.size(); ++trackIndex) {
        const Track& track = m_project->tracks[trackIndex];
        const int trackHeight = effectiveTrackHeight(track);
        p.fillRect(0, y, width(), trackHeight, trackLaneColor(track.type, track.colorIndex));
        y += trackHeight;
    }
    const int contentBottom = y;

    // -------------------------------------------------------------------
    // Alignment grid
    // -------------------------------------------------------------------
    // Faint vertical rules dropped from the ruler's major ticks straight through
    // every lane. This is the single highest-value addition for actually EDITING:
    // it lets you eyeball whether a cut on video lines up with a beat on the
    // music track without dragging the playhead over to check.
    if (contentBottom > kRulerHeight) {
        QColor gridColor = Theme::line();
        gridColor.setAlpha(70);
        p.setPen(QPen(gridColor, 1));
        for (double sec = 0; secToX(sec) < width(); sec += tickIntervalSec) {
            const int x = secToX(sec);
            if (x <= 0) continue;
            p.drawLine(x, kRulerHeight, x, contentBottom);
        }
    }

    // Lane separators, drawn over the grid so lanes stay visually distinct.
    y = kRulerHeight;
    p.setPen(QPen(Theme::lineSoft(), 1));
    for (int trackIndex = 0; trackIndex < m_project->tracks.size(); ++trackIndex) {
        y += effectiveTrackHeight(m_project->tracks[trackIndex]);
        p.drawLine(0, y - 1, width(), y - 1);
    }

    // -------------------------------------------------------------------
    // Clips
    // -------------------------------------------------------------------
    y = kRulerHeight;
    for (int trackIndex = 0; trackIndex < m_project->tracks.size(); ++trackIndex) {
        const Track& track = m_project->tracks[trackIndex];
        const int trackHeight = effectiveTrackHeight(track);
        const ClipPalette palette = paletteFor(track.type, track.colorIndex);

        for (int clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            const Clip& clip = track.clips[clipIndex];
            const bool isSelected = m_selectedClipKeys.contains(clipKey(trackIndex, clipIndex));

            const int x = secToX(clip.trackPosSec);
            const int w = std::max(4, secToX(clip.durationSec()));
            const QRect r(x, y + kClipVMargin, w, trackHeight - 2 * kClipVMargin - 1);
            if (r.right() < 0 || r.left() > width()) continue; // off-screen — skip the work entirely

            const QPainterPath clipPath = roundedPath(r, kClipRadius);

            const int headerH = std::min(kClipHeaderBarHeight, std::max(0, r.height() - 6));
            const QRect headerRect(r.x(), r.y(), r.width(), headerH);
            const QRect bodyRect(r.x(), r.y() + headerH, r.width(), r.height() - headerH);

            // Contact shadow — one pixel of darkness under the clip. Cheap, and
            // it's what makes clips read as objects sitting on the lane rather
            // than holes cut into it.
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 90));
            p.drawPath(roundedPath(r.translated(0, 1), kClipRadius));

            p.save();
            p.setClipPath(clipPath);

            // Body gradient, lighter at the top: a consistent implied light
            // source is most of what separates "designed" from "flat blocks".
            QLinearGradient bodyGradient(bodyRect.topLeft(), bodyRect.bottomLeft());
            bodyGradient.setColorAt(0.0, palette.bodyTop);
            bodyGradient.setColorAt(1.0, palette.bodyBottom);
            p.fillRect(bodyRect, bodyGradient);
            p.fillRect(headerRect, palette.headerColor);

            if (track.type == TrackType::Audio) {
                drawWaveform(p, bodyRect, clip, palette.accent);
            } else if (track.type == TrackType::Video) {
                drawThumbnails(p, bodyRect, clip);

                // Level-of-detail: request a higher-resolution thumbnail strip
                // if the current cached one can't fill this clip's on-screen
                // width densely at the current zoom. The request is cheap to
                // emit liberally — MainWindow debounces it (skips if a regen is
                // already in flight, or if it wouldn't be a meaningful
                // improvement), so there's no need to rate-limit from this side.
                if (!clip.thumbnails.isEmpty() && clip.thumbnailSourceDurationSec > 0.0 && clip.durationSec() > 0.0) {
                    constexpr int kPixelsPerThumbnail = 100; // roughly one distinct frame per this many screen pixels
                    const int desiredCount = std::clamp(bodyRect.width() / kPixelsPerThumbnail, 4, 200);
                    const double fraction = clip.thumbnails.size() / clip.thumbnailSourceDurationSec;
                    const double visibleAtCurrentRes = clip.durationSec() * fraction;
                    if (desiredCount > visibleAtCurrentRes * 1.5) {
                        const double neededFullFileCount = desiredCount
                            * (clip.thumbnailSourceDurationSec / clip.durationSec());
                        const int clamped = std::clamp(static_cast<int>(neededFullFileCount), 12, 200);
                        emit thumbnailDetailNeeded(trackIndex, clipIndex, clamped);
                    }
                }
            }

            // Top inner highlight, one pixel of the track hue lightened — the
            // top edge catching the light.
            p.setPen(QPen(palette.accent.lighter(190), 1));
            p.drawLine(r.left() + kClipRadius, r.top() + 1, r.right() - kClipRadius, r.top() + 1);

            // Disabled tracks dim their clips; muted audio dims a little less,
            // since mute is about sound and disable is about the whole track.
            if (!track.enabled) {
                p.fillRect(r, QColor(Theme::bg0().red(), Theme::bg0().green(), Theme::bg0().blue(), 165));
            } else if (track.muted) {
                p.fillRect(r, QColor(Theme::bg0().red(), Theme::bg0().green(), Theme::bg0().blue(), 120));
            }
            p.restore();

            // Selection: a cyan ring plus a soft outer halo, matching the accent
            // used for every other "this is active" cue in the app. Replaces the
            // old plain white outline, which read as a rendering artifact next to
            // the white-ish waveforms.
            p.setBrush(Qt::NoBrush);
            if (isSelected) {
                QColor halo = Theme::accent();
                halo.setAlpha(70);
                p.setPen(QPen(halo, 4));
                p.drawPath(clipPath);
                p.setPen(QPen(Theme::accent(), 1.6));
                p.drawPath(clipPath);
            } else {
                p.setPen(QPen(QColor(0, 0, 0, 130), 1));
                p.drawPath(clipPath);
            }

            // Trim grips: a pair of vertical bars inset from each edge, only
            // once the clip is wide enough that they aren't the whole clip.
            if (r.width() > 26) {
                p.save();
                p.setClipPath(clipPath);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, isSelected ? 120 : 55));
                const int gripH = std::max(6, r.height() / 3);
                const int gripY = r.center().y() - gripH / 2;
                p.drawRoundedRect(QRectF(r.left() + 2.0, gripY, 2.0, gripH), 1, 1);
                p.drawRoundedRect(QRectF(r.right() - 3.0, gripY, 2.0, gripH), 1, 1);
                p.restore();
            }

            // Keyframe diamonds along the bottom of an animated overlay clip.
            // Without them, animation is completely invisible on the timeline —
            // you'd have to select a clip and read the inspector to find out
            // whether it moves at all, let alone when.
            if (track.type == TrackType::Overlay && clip.anim.hasAnyKeys() && r.width() > 12) {
                p.save();
                p.setClipPath(clipPath);
                const int markerY = r.bottom() - 5;

                // All four properties' keys collapse onto one row: at timeline
                // zoom levels they'd overlap into mush on separate rows, and what
                // matters here is "something changes at this moment", not which
                // property it was. The inspector answers that.
                QVector<double> times;
                for (const auto* prop : {&clip.anim.x, &clip.anim.y, &clip.anim.scale, &clip.anim.opacity}) {
                    for (const auto& key : prop->keys) {
                        bool dup = false;
                        for (double existing : times) {
                            if (std::abs(existing - key.timeSec) < 0.01) { dup = true; break; }
                        }
                        if (!dup) times.push_back(key.timeSec);
                    }
                }

                p.setPen(Qt::NoPen);
                for (double keyTime : times) {
                    const int kx = secToX(clip.trackPosSec + keyTime);
                    if (kx < r.left() - 4 || kx > r.right() + 4) continue;
                    QPolygonF diamond;
                    diamond << QPointF(kx, markerY - 3.5) << QPointF(kx + 3.5, markerY)
                            << QPointF(kx, markerY + 3.5) << QPointF(kx - 3.5, markerY);
                    p.setBrush(QColor(0, 0, 0, 130));
                    p.drawPolygon(diamond.translated(0, 1));
                    p.setBrush(Theme::now());
                    p.drawPolygon(diamond);
                }
                p.restore();
            }

            // Clip label, plus its duration right-aligned when there's room —
            // the duration is genuinely useful while trimming and costs nothing
            // to show, since the header strip is otherwise dead space.
            if (headerH >= 12) {
                p.save();
                p.setClipRect(headerRect);
                const QString name = clip.sourcePath.section('/', -1).section('\\', -1);
                const QString duration = formatTickLabel(clip.durationSec());

                p.setFont(Theme::monoFont(-2));
                const QFontMetrics durFm(p.font());
                const int durW = durFm.horizontalAdvance(duration) + 10;
                const bool showDuration = r.width() > durW + 70;
                if (showDuration) {
                    p.setPen(QColor(255, 255, 255, 130));
                    p.drawText(headerRect.adjusted(0, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight, duration);
                }

                p.setFont(Theme::uiFont(-1, QFont::DemiBold));
                const QFontMetrics nameFm(p.font());
                const int nameAvail = r.width() - 12 - (showDuration ? durW : 0);
                p.setPen(QColor(255, 255, 255, 225));
                p.drawText(headerRect.adjusted(6, 0, -6 - (showDuration ? durW : 0), 0),
                           Qt::AlignVCenter | Qt::AlignLeft,
                           nameFm.elidedText(name, Qt::ElideMiddle, std::max(10, nameAvail)));
                p.restore();
            }
        }

        y += trackHeight;
    }

    // -------------------------------------------------------------------
    // Ruler
    // -------------------------------------------------------------------
    p.fillRect(0, 0, width(), kRulerHeight, Theme::bg2());

    const int tickBottom = kRulerHeight - kMarkerBandHeight - 2;

    // Minor ticks first, so major ticks overdraw them cleanly.
    if (minorIntervalSec * m_pxPerSec >= 7.0) {
        QColor minorColor = Theme::textFaint();
        minorColor.setAlpha(110);
        p.setPen(QPen(minorColor, 1));
        for (double sec = 0; secToX(sec) < width(); sec += minorIntervalSec) {
            const int x = secToX(sec);
            p.drawLine(x, tickBottom - 4, x, tickBottom);
        }
    }

    p.setPen(QPen(Theme::textFaint(), 1));
    for (double sec = 0; secToX(sec) < width(); sec += tickIntervalSec) {
        const int x = secToX(sec);
        p.drawLine(x, tickBottom - 8, x, tickBottom);
    }

    // Tick labels in the mono face — fixed-width digits mean the labels sit on
    // a stable rhythm instead of shifting as the numbers change width.
    p.setFont(Theme::monoFont(-2));
    p.setPen(Theme::textDim());
    for (double sec = 0; secToX(sec) < width(); sec += tickIntervalSec) {
        const int x = secToX(sec);
        p.drawText(QRect(x + 5, 2, 90, 13), Qt::AlignLeft | Qt::AlignVCenter, formatTickLabel(sec));
    }

    p.setPen(QPen(Theme::line(), 1));
    p.drawLine(0, kRulerHeight - 1, width(), kRulerHeight - 1);

    // Markers get their own slim band along the base of the ruler rather than
    // flooding its full height — at full height they swamped the ruler and, in
    // the default amber, competed directly with the playhead for attention.
    for (const auto& marker : m_project->markers) {
        if (marker.isPin()) continue; // pins are drawn separately, below
        const int x1 = secToX(marker.startSec);
        const int x2 = secToX(marker.endSec);
        const QRect band(x1, kRulerHeight - kMarkerBandHeight - 1,
                         std::max(3, x2 - x1), kMarkerBandHeight);
        p.setPen(Qt::NoPen);
        p.setBrush(marker.color);
        p.drawRoundedRect(QRectF(band), 2, 2);

        // A matching translucent wash down the tracks, faint enough to read as
        // a region highlight rather than an object.
        QColor wash = marker.color;
        wash.setAlpha(22);
        p.fillRect(QRect(band.x(), kRulerHeight, band.width(), contentBottom - kRulerHeight), wash);
    }

    // -------------------------------------------------------------------
    // Pins
    // -------------------------------------------------------------------
    // Drawn after the clips so their guide lines lie ON TOP of clip content —
    // the entire reason to drop a pin is to check what lines up with it, which
    // a line hidden behind the clips couldn't do.
    //
    // Deliberately NOT amber: amber means "now" and belongs to the playhead
    // alone. Pins are saved reference positions, the same idea as the snap
    // guides, so they take the same cyan.
    for (const auto& marker : m_project->markers) {
        if (!marker.isPin()) continue;
        const int x = secToX(marker.startSec);
        if (x < -kPinWidth || x > width() + kPinWidth) continue;

        // A dark line first, then the coloured dashes on top of it. Without the
        // underlay the guide disappears wherever it crosses a clip that happens
        // to share its hue — an audio track hue-rotated toward cyan swallowed it
        // completely.
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.drawLine(x, kRulerHeight, x, contentBottom);

        QColor guide = marker.color;
        guide.setAlpha(190);
        p.setPen(QPen(guide, 1, Qt::DashLine));
        p.drawLine(x, kRulerHeight, x, contentBottom);

        // Flag shape: a tag with a point at the bottom, planted on the instant.
        const QRect handle = pinHandleRect(marker.startSec);
        QPainterPath flag;
        flag.moveTo(handle.left(), handle.top() + 2.5);
        flag.quadTo(handle.left(), handle.top(), handle.left() + 2.5, handle.top());
        flag.lineTo(handle.right() - 2.5, handle.top());
        flag.quadTo(handle.right(), handle.top(), handle.right(), handle.top() + 2.5);
        flag.lineTo(handle.right(), handle.bottom() - 5);
        flag.lineTo(handle.center().x() + 0.5, handle.bottom());
        flag.lineTo(handle.left(), handle.bottom() - 5);
        flag.closeSubpath();

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawPath(flag.translated(0, 1)); // contact shadow, as on the clips
        p.setBrush(marker.color);
        p.drawPath(flag);

        // A notch in the flag's face, so a pin still reads as a pin at a glance
        // rather than as a solid cyan blob.
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawRoundedRect(QRectF(handle.center().x() - 0.9, handle.top() + 3.5, 1.8, 5.0), 0.9, 0.9);
    }

    // -------------------------------------------------------------------
    // Interaction overlays
    // -------------------------------------------------------------------
    // Magnetic-snap guide, shown while actively dragging (internal clip drag OR
    // an external file hover) and snapped to something.
    if ((m_drag.mode != DragMode::None || m_isDragHoverActive) && m_activeSnapSec >= 0.0) {
        const int snapX = secToX(m_activeSnapSec);
        QColor glow = Theme::accent();
        glow.setAlpha(60);
        p.setPen(QPen(glow, 5));
        p.drawLine(snapX, kRulerHeight, snapX, contentBottom);
        p.setPen(QPen(Theme::accent(), 1.5));
        p.drawLine(snapX, kRulerHeight, snapX, contentBottom);
    }

    // Ruler hover scrub-preview: a guide line plus a floating time pill, shown
    // only when idly hovering the ruler (not dragging).
    if (m_hoverSec >= 0.0 && m_drag.mode == DragMode::None) {
        const int hx = secToX(m_hoverSec);
        QColor guide = Theme::text();
        guide.setAlpha(90);
        p.setPen(QPen(guide, 1, Qt::DashLine));
        p.drawLine(hx, kRulerHeight, hx, contentBottom);

        const QString label = formatTickLabel(m_hoverSec);
        const QFontMetrics fm(Theme::monoFont(-1, QFont::DemiBold));
        const int labelW = fm.horizontalAdvance(label) + 16;
        const int labelX = std::clamp(hx - labelW / 2, 2, std::max(2, width() - labelW - 2));
        const QRect labelRect(labelX, 3, labelW, kRulerHeight - 11);
        drawTimePill(p, labelRect, label, Theme::bg4(), Theme::text(), labelRect.height() / 2.0);
    }

    // Drag-hover ghost preview — shows where a dragged-in file would land
    // before you release it.
    if (m_isDragHoverActive && m_dragPreviewTrackIndex >= 0
        && m_dragPreviewTrackIndex < m_project->tracks.size()) {
        int laneY = kRulerHeight;
        for (int i = 0; i < m_dragPreviewTrackIndex; ++i) {
            laneY += effectiveTrackHeight(m_project->tracks[i]);
        }
        const int laneHeight = effectiveTrackHeight(m_project->tracks[m_dragPreviewTrackIndex]);
        const QColor ghostAccent = trackAccent(m_project->tracks[m_dragPreviewTrackIndex].type,
                                               m_project->tracks[m_dragPreviewTrackIndex].colorIndex);

        const int x = secToX(m_dragPreviewStartSec);
        const int w = std::max(4, secToX(m_dragPreviewDurationSec));
        const QRect r(x, laneY + kClipVMargin, w, laneHeight - 2 * kClipVMargin - 1);

        const QPainterPath previewPath = roundedPath(r, kClipRadius);
        QColor ghostFill = ghostAccent;
        ghostFill.setAlpha(70);
        p.fillPath(previewPath, ghostFill);
        p.setPen(QPen(Theme::accent(), 1.6, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(previewPath);

        if (!m_dragPreviewFileName.isEmpty()) {
            p.setFont(Theme::uiFont(-1, QFont::DemiBold));
            p.setPen(Theme::text());
            const QFontMetrics fm(p.font());
            p.drawText(r.adjusted(7, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(m_dragPreviewFileName, Qt::ElideMiddle, std::max(10, r.width() - 14)));
        }
    }

    // Frame-preview popup — floats the nearest cached thumbnail for the hovered
    // second above the cursor, like hovering a video player's seek bar. Uses
    // whatever resolution is currently cached (see the level-of-detail system in
    // the clip-drawing loop above), so it sharpens automatically as you zoom in.
    if (m_hoverPreviewTrackIndex >= 0 && m_hoverPreviewClipIndex >= 0
        && m_hoverPreviewTrackIndex < m_project->tracks.size()) {
        const Track& hoverTrack = m_project->tracks[m_hoverPreviewTrackIndex];
        if (m_hoverPreviewClipIndex < hoverTrack.clips.size()) {
            const Clip& hoverClip = hoverTrack.clips[m_hoverPreviewClipIndex];
            if (!hoverClip.thumbnails.isEmpty() && hoverClip.thumbnailSourceDurationSec > 0.0) {
                const double sourceSec = hoverClip.sourceInSec + (m_hoverPreviewSec - hoverClip.trackPosSec);
                const int totalThumbs = hoverClip.thumbnails.size();
                const double fraction = totalThumbs / hoverClip.thumbnailSourceDurationSec;
                int idx = static_cast<int>(sourceSec * fraction);
                idx = std::clamp(idx, 0, totalThumbs - 1);
                const QImage& previewImg = hoverClip.thumbnails[idx];

                if (!previewImg.isNull()) {
                    constexpr int kPreviewW = 168;
                    constexpr int kPreviewH = 94;
                    constexpr int kGapFromCursor = 16;
                    constexpr int kPad = 5;

                    int previewX = m_hoverPreviewMousePos.x() - kPreviewW / 2;
                    previewX = std::clamp(previewX, 6, std::max(6, width() - kPreviewW - 6));
                    int previewY = m_hoverPreviewMousePos.y() - kGapFromCursor - kPreviewH;
                    if (previewY < kRulerHeight) {
                        previewY = m_hoverPreviewMousePos.y() + kGapFromCursor; // flip below rather than clip off-screen
                    }

                    const QRect previewRect(previewX, previewY, kPreviewW, kPreviewH);
                    const QRect cardRect = previewRect.adjusted(-kPad, -kPad, kPad, kPad + 17);

                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(0, 0, 0, 110));
                    p.drawRoundedRect(QRectF(cardRect).translated(0, 2), Theme::kRadiusMd, Theme::kRadiusMd);
                    p.setBrush(Theme::bg2());
                    p.drawRoundedRect(QRectF(cardRect), Theme::kRadiusMd, Theme::kRadiusMd);
                    p.setPen(QPen(Theme::line(), 1));
                    p.setBrush(Qt::NoBrush);
                    p.drawRoundedRect(QRectF(cardRect).adjusted(0.5, 0.5, -0.5, -0.5),
                                      Theme::kRadiusMd, Theme::kRadiusMd);

                    p.save();
                    QPainterPath imgClip;
                    imgClip.addRoundedRect(QRectF(previewRect), 3, 3);
                    p.setClipPath(imgClip);
                    p.drawImage(previewRect, previewImg.scaled(
                        kPreviewW, kPreviewH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                    p.restore();

                    p.setFont(Theme::monoFont(-2, QFont::DemiBold));
                    p.setPen(Theme::textDim());
                    p.drawText(QRect(cardRect.x(), previewRect.bottom() + 2, cardRect.width(), 16),
                               Qt::AlignCenter, formatTickLabel(m_hoverPreviewSec));
                }
            }
        }
    }

    // -------------------------------------------------------------------
    // Playhead
    // -------------------------------------------------------------------
    // The signature element: a hairline in the app's one reserved "now" colour,
    // carrying its own live timecode readout in a capsule at the top. Putting
    // the time ON the needle rather than in a separate label elsewhere means
    // your eye never has to leave the point you're actually working at.
    const int playX = secToX(m_playheadSec);
    const QColor playColor = Theme::now();

    QColor playGlow = playColor;
    playGlow.setAlpha(45);
    p.setPen(QPen(playGlow, 5));
    p.drawLine(playX, kRulerHeight, playX, contentBottom);

    p.setPen(QPen(playColor, 1.4));
    p.drawLine(playX, kRulerHeight - kMarkerBandHeight - 2, playX, contentBottom);

    const QString playLabel = formatPlayheadTimecode(m_playheadSec);
    const QFontMetrics playFm(Theme::monoFont(-1, QFont::DemiBold));
    const int capsuleW = playFm.horizontalAdvance(playLabel) + 18;
    const int capsuleH = kRulerHeight - 11;
    const int capsuleX = std::clamp(playX - capsuleW / 2, 2, std::max(2, width() - capsuleW - 2));
    const QRect capsule(capsuleX, 3, capsuleW, capsuleH);
    drawTimePill(p, capsule, playLabel, playColor, QColor(0x1A, 0x12, 0x02), capsuleH / 2.0);

    // A short stem joining the capsule to the needle, so the readout reads as
    // part of the playhead rather than a label that happens to be nearby.
    p.setPen(QPen(playColor, 1.4));
    p.drawLine(playX, capsule.bottom(), playX, kRulerHeight);
}

QRect Timeline::pinHandleRect(double sec) const {
    // Sits in the ruler, its point resting on the marker band so the flag looks
    // like it's planted at the exact instant it represents.
    const int x = secToX(sec);
    const int bottom = kRulerHeight - 1;
    return QRect(x - kPinWidth / 2, bottom - kPinHeight, kPinWidth, kPinHeight);
}

int Timeline::pinIndexAt(const QPoint& pos) const {
    if (!m_project || pos.y() >= kRulerHeight) return -1;

    // Reverse order so the most recently added pin wins when two overlap at the
    // current zoom — that's the one the user just placed and is most likely
    // reaching for.
    for (int i = m_project->markers.size() - 1; i >= 0; --i) {
        const Marker& marker = m_project->markers[i];
        if (!marker.isPin()) continue; // range markers aren't grabbable this way
        const QRect hit = pinHandleRect(marker.startSec)
                              .adjusted(-kPinHitPadPx, -kPinHitPadPx, kPinHitPadPx, kPinHitPadPx);
        if (hit.contains(pos)) return i;
    }
    return -1;
}

void Timeline::togglePinAtPlayhead() {
    if (!m_project) return;

    // "Already there" is judged in PIXELS, not seconds: at low zoom two pins a
    // second apart would draw on top of each other and be individually
    // unclickable, and at high zoom a fixed seconds-tolerance would refuse
    // legitimately distinct pins. Tying it to what's actually distinguishable
    // on screen is the behaviour that stays right at every zoom level.
    const int playX = secToX(m_playheadSec);
    for (int i = 0; i < m_project->markers.size(); ++i) {
        const Marker& marker = m_project->markers[i];
        if (!marker.isPin()) continue;
        if (std::abs(secToX(marker.startSec) - playX) <= kPinWidth / 2) {
            m_project->markers.remove(i);
            update();
            emit markersChanged();
            return;
        }
    }

    Marker pin;
    pin.startSec = m_playheadSec;
    pin.endSec = m_playheadSec;
    pin.color = Theme::accent();
    m_project->markers.push_back(pin);
    update();
    emit markersChanged();
}

int Timeline::trackIndexAtY(int y) const {
    if (!m_project || y < kRulerHeight) return -1;
    int cursorY = kRulerHeight;
    for (int i = 0; i < m_project->tracks.size(); ++i) {
        const int h = effectiveTrackHeight(m_project->tracks[i]);
        if (y >= cursorY && y < cursorY + h) return i;
        cursorY += h;
    }
    return -1;
}

Timeline::HitResult Timeline::hitTest(const QPoint& pos) const {
    HitResult result;
    if (!m_project || pos.y() < kRulerHeight) return result;

    const int trackIndex = trackIndexAtY(pos.y());
    if (trackIndex < 0) return result;

    const Track& track = m_project->tracks[trackIndex];
    double sec = xToSec(pos.x());

    for (int i = 0; i < track.clips.size(); ++i) {
        const Clip& clip = track.clips[i];
        if (sec >= clip.trackPosSec && sec <= clip.trackPosSec + clip.durationSec()) {
            result.trackIndex = trackIndex;
            result.clipIndex = i;
            return result;
        }
    }
    return result;
}

Timeline::DragMode Timeline::dragModeAt(const QPoint& pos, int trackIndex, int clipIndex) const {
    const Clip& clip = m_project->tracks[trackIndex].clips[clipIndex];
    const int leftX = secToX(clip.trackPosSec);
    const int rightX = secToX(clip.trackPosSec + clip.durationSec());
    if (qAbs(pos.x() - leftX) <= kTrimHandlePx) return DragMode::TrimLeft;
    if (qAbs(pos.x() - rightX) <= kTrimHandlePx) return DragMode::TrimRight;
    return DragMode::MoveClip;
}

QVector<double> Timeline::collectSnapTargets(const QSet<qint64>& excludeKeys) const {
    QVector<double> targets;
    targets.push_back(0.0);
    targets.push_back(m_playheadSec);
    if (m_project) {
        for (int t = 0; t < m_project->tracks.size(); ++t) {
            const Track& track = m_project->tracks[t];
            for (int c = 0; c < track.clips.size(); ++c) {
                if (excludeKeys.contains(clipKey(t, c))) continue;
                const Clip& clip = track.clips[c];
                targets.push_back(clip.trackPosSec);
                targets.push_back(clip.trackPosSec + clip.durationSec());
            }
        }
    }
    return targets;
}

void Timeline::updateCursorForPosition(const QPoint& pos) {
    if (!m_project || pos.y() < kRulerHeight) {
        // A pointing hand over a pin's flag is the only signal that it's a
        // grabbable object rather than part of the ruler graphics.
        setCursor(pinIndexAt(pos) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
    const HitResult hit = hitTest(pos);
    if (hit.clipIndex < 0) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    const DragMode mode = dragModeAt(pos, hit.trackIndex, hit.clipIndex);
    setCursor(mode == DragMode::MoveClip ? Qt::OpenHandCursor : Qt::SizeHorCursor);
}

void Timeline::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-clicking a pin's flag deletes it. Note that Qt delivers a normal
    // press before the double-click, so the first click has already seeked the
    // playhead onto the pin — which makes the removal read as "go there, and
    // now clear it" rather than as something happening at a random position.
    const int pinIndex = pinIndexAt(event->pos());
    if (pinIndex >= 0 && m_project) {
        m_project->markers.remove(pinIndex);
        update();
        emit markersChanged();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void Timeline::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason); // so Delete/Backspace works immediately after clicking a clip
    m_hoverPreviewTrackIndex = -1; // don't leave the scrub-preview popup lingering during a drag
    m_hoverPreviewClipIndex = -1;
    if (event->pos().y() < kRulerHeight) {
        // Clicking a pin seeks exactly to it rather than to wherever in the
        // pin's few pixels the click happened to land — the whole point of
        // dropping a pin is being able to get back to that precise instant.
        const int pinIndex = pinIndexAt(event->pos());
        if (pinIndex >= 0) {
            emit seekRequested(m_project->markers[pinIndex].startSec);
            return;
        }
        emit seekRequested(xToSec(event->pos().x()));
        return;
    }

    const HitResult hit = hitTest(event->pos());
    if (hit.clipIndex < 0) {
        m_drag = DragState{};
        // Clicking empty space clears the selection, UNLESS Ctrl is held
        // (e.g. an accidental empty-space ctrl-click shouldn't wipe a
        // multi-selection the user is building up).
        if (!(event->modifiers() & Qt::ControlModifier) && !m_selectedClipKeys.isEmpty()) {
            m_selectedClipKeys.clear();
            update();
        }
        return;
    }

    const qint64 key = clipKey(hit.trackIndex, hit.clipIndex);

    if (event->modifiers() & Qt::ControlModifier) {
        // Toggle this clip's membership without touching the rest of the
        // selection — standard multi-select convention.
        if (m_selectedClipKeys.contains(key)) {
            m_selectedClipKeys.remove(key);
        } else {
            m_selectedClipKeys.insert(key);
        }
        update();
        emit clipSelected(hit.trackIndex, hit.clipIndex);
        m_drag = DragState{}; // a ctrl-click toggles selection; it doesn't start a drag
        return;
    }

    if (!m_selectedClipKeys.contains(key)) {
        // Fresh click on a clip outside the current selection starts a new
        // single selection. Clicking an ALREADY-selected clip (no
        // modifier) intentionally keeps the whole group selected, so
        // grabbing any member of a multi-selection drags the group.
        m_selectedClipKeys.clear();
        m_selectedClipKeys.insert(key);
    }
    update();
    emit clipSelected(hit.trackIndex, hit.clipIndex);

    const Clip& clip = m_project->tracks[hit.trackIndex].clips[hit.clipIndex];
    m_drag.mode = dragModeAt(event->pos(), hit.trackIndex, hit.clipIndex);
    m_drag.primaryTrackIndex = hit.trackIndex;
    m_drag.primaryClipIndex = hit.clipIndex;
    m_drag.startMousePos = event->pos();
    m_drag.startSourceInSec = clip.sourceInSec;
    m_drag.startSourceOutSec = clip.sourceOutSec;

    m_drag.movingClips.clear();
    QSet<qint64> excludeKeys;
    if (m_drag.mode == DragMode::MoveClip) {
        // Snapshot every selected clip's starting position so the whole
        // group moves together, preserving relative spacing.
        for (qint64 k : m_selectedClipKeys) {
            const int t = static_cast<int>(k >> 32);
            const int c = static_cast<int>(k & 0xffffffffLL);
            if (t >= 0 && t < m_project->tracks.size() && c >= 0 && c < m_project->tracks[t].clips.size()) {
                m_drag.movingClips.push_back({t, c, m_project->tracks[t].clips[c].trackPosSec});
            }
        }
        excludeKeys = m_selectedClipKeys; // don't snap the group against its own members
    } else {
        excludeKeys.insert(key); // trimming only ever affects the primary clip
    }

    m_snapTargets = collectSnapTargets(excludeKeys);
}

void Timeline::mouseMoveEvent(QMouseEvent* event) {
    // Ruler scrubbing (drag to seek) — unchanged behavior for the ruler band.
    if ((event->buttons() & Qt::LeftButton) && m_drag.mode == DragMode::None
        && event->pos().y() < kRulerHeight) {
        emit seekRequested(xToSec(event->pos().x()));
        return;
    }

    // Idle hover: scrub-preview on the ruler, cursor feedback over clips,
    // and the frame-preview popup while hovering a video clip's body.
    if (event->buttons() == Qt::NoButton) {
        m_hoverSec = (event->pos().y() < kRulerHeight) ? xToSec(event->pos().x()) : -1.0;
        updateCursorForPosition(event->pos());

        m_hoverPreviewTrackIndex = -1;
        m_hoverPreviewClipIndex = -1;
        if (m_project && event->pos().y() >= kRulerHeight) {
            const HitResult hit = hitTest(event->pos());
            if (hit.clipIndex >= 0 && m_project->tracks[hit.trackIndex].type == TrackType::Video) {
                m_hoverPreviewTrackIndex = hit.trackIndex;
                m_hoverPreviewClipIndex = hit.clipIndex;
                m_hoverPreviewSec = xToSec(event->pos().x());
                m_hoverPreviewMousePos = event->pos();
            }
        }

        update();
        return;
    }

    if (m_drag.mode == DragMode::None || !m_project) return;

    const double deltaSec = xToSec(event->pos().x()) - xToSec(m_drag.startMousePos.x());
    const double snapThresholdSec = kSnapThresholdPx / m_pxPerSec;
    Clip& primaryClip = m_project->tracks[m_drag.primaryTrackIndex].clips[m_drag.primaryClipIndex];
    m_activeSnapSec = -1.0;

    if (m_drag.mode == DragMode::MoveClip) {
        // Find the primary clip's own starting position within the group
        // snapshot, so the delta/snap math below is computed relative to
        // the clip actually grabbed — then the SAME final delta is applied
        // to every other clip in the group, preserving their spacing.
        double primaryStartTrackPosSec = 0.0;
        for (const auto& mc : m_drag.movingClips) {
            if (mc.trackIndex == m_drag.primaryTrackIndex && mc.clipIndex == m_drag.primaryClipIndex) {
                primaryStartTrackPosSec = mc.startTrackPosSec;
                break;
            }
        }

        const double duration = m_drag.startSourceOutSec - m_drag.startSourceInSec;
        double newStart = std::max(0.0, primaryStartTrackPosSec + deltaSec);
        const double newEnd = newStart + duration;

        double bestDist = snapThresholdSec;
        bool snapped = false;
        double snappedStart = newStart;
        for (double target : m_snapTargets) {
            const double d = std::fabs(newStart - target);
            if (d <= bestDist) { bestDist = d; snappedStart = target; snapped = true; }
        }
        if (!snapped) {
            bestDist = snapThresholdSec;
            for (double target : m_snapTargets) {
                const double d = std::fabs(newEnd - target);
                if (d <= bestDist) { bestDist = d; snappedStart = target - duration; snapped = true; }
            }
        }
        if (snapped) {
            newStart = std::max(0.0, snappedStart);
            m_activeSnapSec = newStart;
        }

        // The FINAL applied offset (post-snap, post-clamp) — applying this
        // same delta to every group member is what keeps their relative
        // spacing intact rather than each one independently snapping.
        const double actualDelta = newStart - primaryStartTrackPosSec;

        for (const auto& mc : m_drag.movingClips) {
            if (mc.trackIndex < m_project->tracks.size()) {
                auto& clips = m_project->tracks[mc.trackIndex].clips;
                if (mc.clipIndex < clips.size()) {
                    clips[mc.clipIndex].trackPosSec = std::max(0.0, mc.startTrackPosSec + actualDelta);
                }
            }
        }

    } else if (m_drag.mode == DragMode::TrimLeft) {
        const double startTrackPosSec = primaryClip.trackPosSec + (m_drag.startSourceInSec - primaryClip.sourceInSec);
        double newIn = std::clamp(m_drag.startSourceInSec + deltaSec, 0.0,
                                   m_drag.startSourceOutSec - kMinClipLenSec);
        double newTrackPos = startTrackPosSec + (newIn - m_drag.startSourceInSec);

        double bestDist = snapThresholdSec;
        bool snapped = false;
        double snappedPos = newTrackPos;
        for (double target : m_snapTargets) {
            const double d = std::fabs(newTrackPos - target);
            if (d <= bestDist) { bestDist = d; snappedPos = target; snapped = true; }
        }
        if (snapped) {
            newIn = std::clamp(m_drag.startSourceInSec + (snappedPos - startTrackPosSec),
                                0.0, m_drag.startSourceOutSec - kMinClipLenSec);
            newTrackPos = startTrackPosSec + (newIn - m_drag.startSourceInSec);
            m_activeSnapSec = newTrackPos;
        }
        primaryClip.sourceInSec = newIn;
        primaryClip.trackPosSec = std::max(0.0, newTrackPos);

    } else if (m_drag.mode == DragMode::TrimRight) {
        // Cap at the source file's own length if known (from whichever of
        // waveform/thumbnail metadata is present); otherwise allow generous
        // growth rather than blocking the gesture entirely.
        const double sourceLimit = primaryClip.waveformSourceDurationSec > 0.0 ? primaryClip.waveformSourceDurationSec
                                  : (primaryClip.thumbnailSourceDurationSec > 0.0 ? primaryClip.thumbnailSourceDurationSec : 1e9);
        const double startTrackPosSec = primaryClip.trackPosSec; // unchanged by right-trim
        double newOut = std::clamp(m_drag.startSourceOutSec + deltaSec,
                                    m_drag.startSourceInSec + kMinClipLenSec, sourceLimit);
        const double newEndPos = startTrackPosSec + (newOut - m_drag.startSourceInSec);

        double bestDist = snapThresholdSec;
        bool snapped = false;
        double snappedEnd = newEndPos;
        for (double target : m_snapTargets) {
            const double d = std::fabs(newEndPos - target);
            if (d <= bestDist) { bestDist = d; snappedEnd = target; snapped = true; }
        }
        if (snapped) {
            newOut = std::clamp(m_drag.startSourceInSec + (snappedEnd - startTrackPosSec),
                                 m_drag.startSourceInSec + kMinClipLenSec, sourceLimit);
            m_activeSnapSec = startTrackPosSec + (newOut - m_drag.startSourceInSec);
        }
        primaryClip.sourceOutSec = newOut;
    }

    updateGeometry(); // duration may have grown/shrunk — let the scroll area know
    update();
}

void Timeline::mouseReleaseEvent(QMouseEvent*) {
    m_drag = DragState{};
    m_activeSnapSec = -1.0;
    update();
}

void Timeline::leaveEvent(QEvent*) {
    m_hoverSec = -1.0;
    m_hoverPreviewTrackIndex = -1;
    m_hoverPreviewClipIndex = -1;
    update();
}

void Timeline::setSelectedClip(int trackIndex, int clipIndex) {
    const qint64 key = clipKey(trackIndex, clipIndex);
    if (m_selectedClipKeys.size() == 1 && m_selectedClipKeys.contains(key)) return;
    m_selectedClipKeys.clear();
    m_selectedClipKeys.insert(key);
    update();
}

void Timeline::wheelEvent(QWheelEvent* event) {
    // Ctrl+scroll zooms the timeline in/out, anchored on the cursor rather
    // than always zooming from the start of the timeline. Plain scroll is
    // left to propagate to the enclosing QScrollArea for normal scrolling.
    if (event->modifiers() & Qt::ControlModifier) {
        // The second currently under the cursor, and its OLD pixel-space
        // x-coordinate — captured before changing zoom, so MainWindow can
        // work out how much to shift the scrollbar afterward to keep this
        // exact point visually under the cursor.
        const double anchorSec = xToSec(event->position().toPoint().x());
        const int oldPixelX = event->position().toPoint().x();

        const double factor = (event->angleDelta().y() > 0) ? 1.15 : (1.0 / 1.15);
        setPixelsPerSecond(m_pxPerSec * factor);

        emit zoomAnchorChanged(anchorSec, oldPixelX);
        event->accept();
        return;
    }

    if (event->modifiers() & Qt::ShiftModifier) {
        // Shift+scroll = horizontal scroll, the standard convention for a
        // plain vertical mouse wheel — Qt doesn't reliably remap this to
        // the horizontal scrollbar on its own (that's more of a
        // trackpad/OS-level convention than a guaranteed QScrollArea
        // behavior), so it's handled explicitly here. Timeline doesn't own
        // the scroll area itself, so it emits the request for MainWindow
        // to apply — same pattern as zoomAnchorChanged above.
        //
        // Prefer pixelDelta() when available (trackpads/high-res mice
        // report this directly, in actual pixels); fall back to a scaled
        // angleDelta() for a standard wheel, where one "click" = 120 units.
        int deltaPixels = event->pixelDelta().y();
        if (deltaPixels == 0) {
            deltaPixels = static_cast<int>((event->angleDelta().y() / 120.0) * 80);
        }
        emit horizontalScrollRequested(-deltaPixels);
        event->accept();
        return;
    }

    QWidget::wheelEvent(event);
}

void Timeline::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) return;

    // Probe the first file's duration up front — cheap (just reads the
    // container header, no decoding), so it's fine to do once here without
    // any lag, and lets the hover preview show a realistic width instead of
    // an arbitrary placeholder.
    m_dragPreviewDurationSec = 5.0;
    m_dragPreviewFileName.clear();
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        const double probed = MediaProbe::probeDurationSeconds(path);
        if (probed > 0.0) m_dragPreviewDurationSec = probed;
        m_dragPreviewFileName = QFileInfo(path).fileName();
        break; // only preview the first file even if multiple are dragged
    }

    m_isDragHoverActive = true;
    event->acceptProposedAction();
}

void Timeline::dragMoveEvent(QDragMoveEvent* event) {
    if (!event->mimeData()->hasUrls()) return;

    const QPoint pos = event->position().toPoint();
    // Centered on the cursor, not starting AT it — otherwise the cursor
    // (and the OS's own drag thumbnail) sits directly on the bar's start
    // edge, hiding exactly the point you need to see to place it accurately.
    double centeredStart = std::max(0.0, xToSec(pos.x()) - m_dragPreviewDurationSec / 2.0);
    const double centeredEnd = centeredStart + m_dragPreviewDurationSec;

    // Same magnetic snapping as an internal clip drag — snap either edge to
    // another clip's start/end or the playhead. Nothing is excluded since
    // there's no "self" clip yet at this point.
    const double snapThresholdSec = kSnapThresholdPx / m_pxPerSec;
    const QVector<double> targets = collectSnapTargets(QSet<qint64>());
    m_activeSnapSec = -1.0;

    double bestDist = snapThresholdSec;
    bool snapped = false;
    double snappedStart = centeredStart;
    for (double target : targets) {
        const double d = std::fabs(centeredStart - target);
        if (d <= bestDist) { bestDist = d; snappedStart = target; snapped = true; }
    }
    if (!snapped) {
        bestDist = snapThresholdSec;
        for (double target : targets) {
            const double d = std::fabs(centeredEnd - target);
            if (d <= bestDist) { bestDist = d; snappedStart = target - m_dragPreviewDurationSec; snapped = true; }
        }
    }
    if (snapped) {
        centeredStart = std::max(0.0, snappedStart);
        m_activeSnapSec = centeredStart;
    }

    m_dragPreviewStartSec = centeredStart;
    m_dragPreviewTrackIndex = std::max(0, trackIndexAtY(pos.y()));
    update();
    event->acceptProposedAction();
}

void Timeline::dragLeaveEvent(QDragLeaveEvent*) {
    m_isDragHoverActive = false;
    m_activeSnapSec = -1.0;
    update();
}

void Timeline::dropEvent(QDropEvent* event) {
    m_isDragHoverActive = false;
    m_activeSnapSec = -1.0;
    if (!m_project || !event->mimeData()->hasUrls()) { update(); return; }

    // Reuses the exact position dragMoveEvent last computed (centered AND
    // snapped) rather than recomputing from the raw cursor — guarantees the
    // clip lands exactly where the ghost preview showed, snap included.
    const double timelineSec = m_dragPreviewStartSec;
    const int trackIndex = std::max(0, m_dragPreviewTrackIndex);

    bool acceptedAny = false;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        emit mediaDropped(url.toLocalFile(), trackIndex, timelineSec);
        acceptedAny = true;
    }

    if (acceptedAny) {
        event->acceptProposedAction();
    }
    update(); // clear the ghost preview
}

void Timeline::deleteClip(int trackIndex, int clipIndex) {
    if (!m_project || trackIndex < 0 || trackIndex >= m_project->tracks.size()) return;
    Track& track = m_project->tracks[trackIndex];
    if (clipIndex < 0 || clipIndex >= track.clips.size()) return;

    track.clips.removeAt(clipIndex);
    m_selectedClipKeys.remove(clipKey(trackIndex, clipIndex));
    updateGeometry(); // duration may have shrunk — let the scroll area know
    update();
    emit clipDeleted();
}

void Timeline::deleteSelectedClips() {
    if (!m_project || m_selectedClipKeys.isEmpty()) return;

    // Group selected clip indices by track, then within each track process
    // in DESCENDING clip-index order — removing a lower-index clip would
    // otherwise shift the indices of higher-index ones still pending
    // removal on the same track.
    QHash<int, QVector<int>> byTrack;
    for (qint64 key : m_selectedClipKeys) {
        const int t = static_cast<int>(key >> 32);
        const int c = static_cast<int>(key & 0xffffffffLL);
        byTrack[t].push_back(c);
    }

    for (auto it = byTrack.begin(); it != byTrack.end(); ++it) {
        if (it.key() < 0 || it.key() >= m_project->tracks.size()) continue;
        std::sort(it.value().begin(), it.value().end(), std::greater<int>());
        Track& track = m_project->tracks[it.key()];
        for (int clipIdx : it.value()) {
            if (clipIdx >= 0 && clipIdx < track.clips.size()) {
                track.clips.removeAt(clipIdx);
            }
        }
    }

    m_selectedClipKeys.clear();
    updateGeometry();
    update();
    emit clipDeleted();
}

void Timeline::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_M) {
        togglePinAtPlayhead();
        return;
    }

    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && !m_selectedClipKeys.isEmpty()) {
        deleteSelectedClips();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void Timeline::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_project) return;
    const HitResult hit = hitTest(event->pos());
    if (hit.clipIndex < 0) return;

    const qint64 key = clipKey(hit.trackIndex, hit.clipIndex);
    // Right-clicking a clip that's NOT already part of the selection
    // replaces the selection with just that clip — matching left-click.
    // Right-clicking an already-selected clip leaves the whole group
    // selected, so "Delete" in the menu acts on the full group.
    if (!m_selectedClipKeys.contains(key)) {
        m_selectedClipKeys.clear();
        m_selectedClipKeys.insert(key);
        update();
        emit clipSelected(hit.trackIndex, hit.clipIndex);
    }

    QMenu menu(this);
    const QString label = m_selectedClipKeys.size() > 1
        ? QString("Delete %1 Clips").arg(m_selectedClipKeys.size())
        : "Delete Clip";
    QAction* deleteAction = menu.addAction(label);
    if (menu.exec(event->globalPos()) == deleteAction) {
        deleteSelectedClips();
    }
}
