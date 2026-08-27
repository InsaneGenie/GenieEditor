#include "TrackHeaderPanel.h"
#include "TimelineMetrics.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QWidgetAction>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QLineEdit>
#include <algorithm>

using namespace TimelineMetrics;

TrackHeaderPanel::TrackHeaderPanel(QWidget* parent) : QWidget(parent) {
    setFixedWidth(kTrackHeaderWidth);
    setMouseTracking(true); // needed for hover cursor feedback on resize handles
}

void TrackHeaderPanel::setProject(Project* project) {
    m_project = project;
    updateGeometry();
    // The containing QScrollArea uses setWidgetResizable(false) (see
    // MainWindow — it's driven programmatically to stay in sync with
    // Timeline's own scrollbar rather than scrolled directly by the user),
    // which means Qt does NOT auto-resize this widget when sizeHint()
    // grows — that auto-management only happens when widgetResizable is
    // true. updateGeometry() alone only schedules a layout *request*.
    // setMinimumHeight is a stronger, more reliable signal than resize()
    // alone: it's a hard constraint on minimumSizeHint() that persists
    // (rather than a one-time size assignment that something else could
    // still override), guaranteeing the scrollbar range actually reflects
    // newly added or resized tracks.
    setMinimumHeight(sizeHint().height());
    resize(sizeHint());
    update();
}

QSize TrackHeaderPanel::sizeHint() const {
    if (!m_project) return QSize(kTrackHeaderWidth, kRulerHeight + kTrackHeight);
    return QSize(kTrackHeaderWidth, kRulerHeight + totalTracksHeight(*m_project));
}

int TrackHeaderPanel::trackIndexAt(int y) const {
    if (!m_project || y < kRulerHeight) return -1;
    int cursorY = kRulerHeight;
    for (int i = 0; i < m_project->tracks.size(); ++i) {
        const int h = effectiveTrackHeight(m_project->tracks[i]);
        if (y >= cursorY && y < cursorY + h) return i;
        cursorY += h;
    }
    return -1;
}

// --- Inline button geometry -------------------------------------------------
// Larger and more generously spaced than the old 22x18 squares — these are the
// controls you reach for constantly while mixing, and they were previously
// small enough to be a genuine aiming problem.
namespace {
constexpr int kBtnW = 26;
constexpr int kBtnH = 22;
constexpr int kBtnGap = 5;
constexpr int kBtnLeft = 13;
constexpr int kBtnBottomInset = 30;
// Below this height the button row is suppressed rather than allowed to
// collide with the name above it.
constexpr int kMinHeightForButtons = 52;
} // namespace

QRect TrackHeaderPanel::muteButtonRect(int trackIndex) const {
    if (!m_project) return QRect();
    const int y = trackTopY(*m_project, trackIndex);
    const int h = effectiveTrackHeight(m_project->tracks[trackIndex]);
    return QRect(kBtnLeft, y + h - kBtnBottomInset, kBtnW, kBtnH);
}

QRect TrackHeaderPanel::disableButtonRect(int trackIndex) const {
    if (!m_project) return QRect();
    const int y = trackTopY(*m_project, trackIndex);
    const int h = effectiveTrackHeight(m_project->tracks[trackIndex]);
    // Sits right after the mute button for audio tracks; starts at the same
    // left position mute would've used for video/overlay tracks (which have no
    // mute button at all).
    const bool hasMute = (m_project->tracks[trackIndex].type == TrackType::Audio);
    const int x = hasMute ? kBtnLeft + kBtnW + kBtnGap : kBtnLeft;
    return QRect(x, y + h - kBtnBottomInset, kBtnW, kBtnH);
}

QRect TrackHeaderPanel::deleteButtonRect(int trackIndex) const {
    const QRect disableRect = disableButtonRect(trackIndex);
    return QRect(disableRect.right() + 1 + kBtnGap, disableRect.y(), kBtnW, kBtnH);
}

QRect TrackHeaderPanel::nameLabelRect(int trackIndex) const {
    if (!m_project) return QRect();
    const int y = trackTopY(*m_project, trackIndex);
    return QRect(kBtnLeft, y + 7, width() - kBtnLeft - 10, 18);
}

TrackHeaderPanel::HeaderButton TrackHeaderPanel::buttonAt(const QPoint& pos, int trackIndex) const {
    if (!m_project || trackIndex < 0 || trackIndex >= m_project->tracks.size()) return HeaderButton::None;
    if (effectiveTrackHeight(m_project->tracks[trackIndex]) < kMinHeightForButtons) return HeaderButton::None;

    if (m_project->tracks[trackIndex].type == TrackType::Audio
        && muteButtonRect(trackIndex).contains(pos)) {
        return HeaderButton::Mute;
    }
    if (disableButtonRect(trackIndex).contains(pos)) return HeaderButton::Disable;
    if (deleteButtonRect(trackIndex).contains(pos)) return HeaderButton::Delete;
    return HeaderButton::None;
}

bool TrackHeaderPanel::isNearBottomEdge(const QPoint& pos, int trackIndex) const {
    if (!m_project) return false;
    const int y = trackTopY(*m_project, trackIndex);
    const int h = effectiveTrackHeight(m_project->tracks[trackIndex]);
    return qAbs(pos.y() - (y + h)) <= kResizeHandlePx;
}

void TrackHeaderPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Theme::bg1());

    // Top-left corner cell, matching Timeline's ruler strip so the seam between
    // the two widgets is invisible — including its bottom border, which now
    // reads as one continuous line running under both.
    p.fillRect(0, 0, width(), kRulerHeight, Theme::bg2());
    p.setPen(QPen(Theme::line(), 1));
    p.drawLine(0, kRulerHeight - 1, width(), kRulerHeight - 1);

    if (!m_project) return;

    // A "TRACKS" label in the corner cell, set to match the timeline ruler's
    // own tick labels so the two halves of the header row read as one strip.
    QFont cornerFont = Theme::uiFont(-2, QFont::DemiBold);
    cornerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    p.setFont(cornerFont);
    p.setPen(Theme::textFaint());
    p.drawText(QRect(kBtnLeft, 0, width() - kBtnLeft, kRulerHeight - 1),
               Qt::AlignVCenter | Qt::AlignLeft, "TRACKS");

    int y = kRulerHeight;
    for (int i = 0; i < m_project->tracks.size(); ++i) {
        const Track& track = m_project->tracks[i];
        const int h = effectiveTrackHeight(track);
        const bool isAudio = (track.type == TrackType::Audio);
        const QColor accent = trackAccent(track.type, track.colorIndex);

        // Header lane is a touch lighter than the timeline's matching lane, so
        // the frozen column reads as raised chrome rather than as more canvas.
        p.fillRect(0, y, width(), h, trackLaneColor(track.type, track.colorIndex).lighter(118));

        // Colored accent stripe on the left edge — the same hue this track's
        // clips use on the timeline, which is what visually ties a header to
        // its lane across the scroll seam.
        p.fillRect(0, y, 3, h, accent);

        // Disabled tracks get an overall dimming so it's obvious at a glance
        // without needing to read the button state.
        if (!track.enabled) {
            p.fillRect(3, y, width() - 3, h, QColor(Theme::bg0().red(), Theme::bg0().green(),
                                                    Theme::bg0().blue(), 140));
        }

        // Track name.
        if (i != m_renamingTrackIndex) { // the QLineEdit covers this while renaming
            p.setFont(Theme::uiFont(0, QFont::DemiBold));
            p.setPen(track.enabled ? Theme::text() : Theme::textFaint());
            const QFontMetrics fm(p.font());
            const QRect nameRect = nameLabelRect(i);
            p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(track.name, Qt::ElideRight, nameRect.width()));
        }

        // Type label, in the track's own colour and letterspaced small caps —
        // it's a category marker, not prose, and setting it as one keeps it from
        // competing with the track name directly above.
        QFont typeFont = Theme::uiFont(-2, QFont::DemiBold);
        typeFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        p.setFont(typeFont);
        p.setPen(track.enabled ? accent.lighter(135) : Theme::textFaint());
        const QString typeLabel = (track.type == TrackType::Video) ? "VIDEO"
                                : (track.type == TrackType::Overlay) ? "OVERLAY" : "AUDIO";
        p.drawText(QRect(kBtnLeft, y + 25, width() - kBtnLeft - 10, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, typeLabel);

        // Audio tracks show their gain right-aligned on the same line. It's set
        // from the right-click menu and was previously invisible afterwards —
        // there was no way to tell a track was turned down without reopening
        // the menu to look.
        if (isAudio && h >= 40) {
            p.setFont(Theme::monoFont(-2));
            p.setPen(track.volumePercent == 100 ? Theme::textFaint() : Theme::accent());
            p.drawText(QRect(0, y + 25, width() - 12, 14), Qt::AlignRight | Qt::AlignVCenter,
                       QString("%1%").arg(track.volumePercent));
        }

        // --- Inline buttons --------------------------------------------------
        if (h >= kMinHeightForButtons) {
            auto drawButton = [&](const QRect& r, HeaderButton which, Theme::Icon iconId,
                                  bool active, const QColor& activeColor) {
                const bool hovered = (m_hoverButtonTrack == i && m_hoverButton == which);

                QColor fill = active ? activeColor : Theme::bg3();
                if (hovered) fill = active ? activeColor.lighter(118) : Theme::bg4();

                p.setPen(QPen(active ? activeColor.lighter(130)
                                     : (hovered ? Theme::line().lighter(150) : Theme::line()), 1));
                p.setBrush(fill);
                p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5),
                                  Theme::kRadiusSm, Theme::kRadiusSm);

                const QColor iconColor = active ? QColor(0x10, 0x13, 0x18)
                                                : (hovered ? Theme::text() : Theme::textDim());
                const int iconSize = 14;
                const QRect iconRect(r.center().x() - iconSize / 2 + 1,
                                     r.center().y() - iconSize / 2 + 1, iconSize, iconSize);
                Theme::icon(iconId, iconColor, iconSize).paint(&p, iconRect);
            };

            if (isAudio) {
                drawButton(muteButtonRect(i), HeaderButton::Mute,
                           track.muted ? Theme::Icon::VolumeMute : Theme::Icon::Volume,
                           track.muted, Theme::danger());
            }
            drawButton(disableButtonRect(i), HeaderButton::Disable,
                       track.enabled ? Theme::Icon::Eye : Theme::Icon::EyeOff,
                       !track.enabled, Theme::warning());
            drawButton(deleteButtonRect(i), HeaderButton::Delete, Theme::Icon::Trash,
                       false, Theme::danger());
        }

        // Separator line at the bottom of the lane, doubling as the visual cue
        // for the resize handle.
        p.setPen(QPen(Theme::lineSoft(), 1));
        p.drawLine(0, y + h - 1, width(), y + h - 1);

        y += h;
    }
}

void TrackHeaderPanel::mousePressEvent(QMouseEvent* event) {
    if (!m_project) return;
    const int trackIndex = trackIndexAt(event->pos().y());
    if (trackIndex < 0) return;

    if (isNearBottomEdge(event->pos(), trackIndex)) {
        m_resizingTrackIndex = trackIndex;
        m_resizeStartY = event->pos().y();
        m_resizeStartHeight = effectiveTrackHeight(m_project->tracks[trackIndex]);
        return;
    }

    switch (buttonAt(event->pos(), trackIndex)) {
    case HeaderButton::Mute:
        m_project->tracks[trackIndex].muted = !m_project->tracks[trackIndex].muted;
        update();
        emit muteToggled(trackIndex);
        return;
    case HeaderButton::Disable:
        m_project->tracks[trackIndex].enabled = !m_project->tracks[trackIndex].enabled;
        update();
        emit trackEnabledChanged(trackIndex);
        return;
    case HeaderButton::Delete:
        emit deleteTrackRequested(trackIndex); // MainWindow performs the actual removal — see the signal's doc comment
        return;
    case HeaderButton::None:
        break;
    }
}

void TrackHeaderPanel::mouseMoveEvent(QMouseEvent* event) {
    if (m_resizingTrackIndex >= 0 && m_project) {
        const int delta = event->pos().y() - m_resizeStartY;
        const int newHeight = std::clamp(m_resizeStartHeight + delta, kMinTrackHeight, kMaxTrackHeight);
        m_project->tracks[m_resizingTrackIndex].heightPx = newHeight;
        updateGeometry();
        setMinimumHeight(sizeHint().height()); // see setProject() for why this explicit call is necessary here
        resize(sizeHint());
        update();
        emit trackHeightChanged();
        return;
    }

    const int trackIndex = trackIndexAt(event->pos().y());
    const bool onResizeHandle = (trackIndex >= 0 && isNearBottomEdge(event->pos(), trackIndex));

    // Resize handle wins over button hover: the handle sits on the lane's bottom
    // edge, which can overlap the button row on a short track, and getting the
    // resize cursor there is the more useful of the two.
    const HeaderButton hovered = onResizeHandle ? HeaderButton::None
                                                : buttonAt(event->pos(), trackIndex);
    if (hovered != m_hoverButton || trackIndex != m_hoverButtonTrack) {
        m_hoverButton = hovered;
        m_hoverButtonTrack = trackIndex;
        update();
    }

    setCursor(onResizeHandle ? Qt::SizeVerCursor
                             : (hovered != HeaderButton::None ? Qt::PointingHandCursor : Qt::ArrowCursor));
}

void TrackHeaderPanel::leaveEvent(QEvent* event) {
    // Without this the last-hovered button stays lit after the pointer has left
    // the panel entirely.
    if (m_hoverButton != HeaderButton::None) {
        m_hoverButton = HeaderButton::None;
        m_hoverButtonTrack = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void TrackHeaderPanel::mouseReleaseEvent(QMouseEvent*) {
    m_resizingTrackIndex = -1;
}

void TrackHeaderPanel::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!m_project) return;
    const int trackIndex = trackIndexAt(event->pos().y());
    if (trackIndex < 0) return;

    // Only start renaming if the double-click actually landed on the
    // name text itself, not the type label, buttons, or resize handle
    // below it — avoids accidentally triggering a rename from a fast
    // double-click elsewhere in the row.
    if (!nameLabelRect(trackIndex).contains(event->pos())) return;

    startRenaming(trackIndex);
}

void TrackHeaderPanel::startRenaming(int trackIndex) {
    if (!m_project || trackIndex < 0 || trackIndex >= m_project->tracks.size()) return;

    if (!m_renameEdit) {
        m_renameEdit = new QLineEdit(this);
        m_renameEdit->setFont(Theme::uiFont(0, QFont::DemiBold));
        // Picks up the global QLineEdit styling (including the accent focus
        // ring) instead of the hardcoded grey it used to carry.
        connect(m_renameEdit, &QLineEdit::editingFinished, this, &TrackHeaderPanel::finishRenaming);
    }

    m_renamingTrackIndex = trackIndex;
    const QRect nameRect = nameLabelRect(trackIndex);
    m_renameEdit->setGeometry(nameRect.adjusted(-2, -2, 2, 2));
    m_renameEdit->setText(m_project->tracks[trackIndex].name);
    m_renameEdit->selectAll();
    m_renameEdit->show();
    m_renameEdit->setFocus();
    update(); // hide the static name text underneath immediately
}

void TrackHeaderPanel::finishRenaming() {
    if (m_renameEdit) m_renameEdit->hide();

    if (m_project && m_renamingTrackIndex >= 0 && m_renamingTrackIndex < m_project->tracks.size()) {
        const QString newName = m_renameEdit->text().trimmed();
        if (!newName.isEmpty()) {
            m_project->tracks[m_renamingTrackIndex].name = newName;
            emit trackRenamed(m_renamingTrackIndex);
        }
    }

    m_renamingTrackIndex = -1;
    update();
}

void TrackHeaderPanel::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_project) return;
    const int trackIndex = trackIndexAt(event->pos().y());
    if (trackIndex < 0 || m_project->tracks[trackIndex].type != TrackType::Audio) return;

    QMenu menu(this);

    // A slider embedded directly in the menu (via QWidgetAction) — the
    // standard Qt pattern for popup volume controls, e.g. system tray
    // volume flyouts — rather than a separate dialog, so it feels like a
    // quick in-place adjustment rather than an interruption.
    auto* sliderWidget = new QWidget(&menu);
    auto* layout = new QHBoxLayout(sliderWidget);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->addWidget(new QLabel("Volume:", sliderWidget));

    auto* slider = new QSlider(Qt::Horizontal, sliderWidget);
    slider->setRange(0, 200);
    slider->setValue(m_project->tracks[trackIndex].volumePercent);
    slider->setFixedWidth(150);
    layout->addWidget(slider);

    auto* valueLabel = new QLabel(QString("%1%").arg(slider->value()), sliderWidget);
    valueLabel->setFixedWidth(40);
    layout->addWidget(valueLabel);

    connect(slider, &QSlider::valueChanged, this, [this, trackIndex, valueLabel](int value) {
        if (!m_project || trackIndex >= m_project->tracks.size()) return;
        m_project->tracks[trackIndex].volumePercent = value;
        valueLabel->setText(QString("%1%").arg(value));
        emit trackVolumeChanged(trackIndex);
    });

    auto* sliderAction = new QWidgetAction(&menu);
    sliderAction->setDefaultWidget(sliderWidget);
    menu.addAction(sliderAction);

    menu.exec(event->globalPos());
}
