#include "OverlayInspectorPanel.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QToolButton>
#include <QSpinBox>
#include <QPushButton>
#include <QStackedLayout>
#include <QFrame>
#include <QScrollArea>
#include <QMenu>
#include <QContextMenuEvent>
#include <QPixmap>
#include <QImage>
#include "OverlayImageLoader.h"
#include <algorithm>
#include <cmath>

namespace {
// Sliders work in integers, so every property is edited as a percentage and
// converted at the boundary. Position runs past 0-100 deliberately: an overlay
// has to be able to sit fully off-canvas for a slide-in to have somewhere to
// slide in FROM.
constexpr int kPosMin = -50;
constexpr int kPosMax = 150;
constexpr int kScaleMin = 1;
constexpr int kScaleMax = 200;
constexpr int kOpacityMin = 0;
constexpr int kOpacityMax = 100;
constexpr int kRotationMin = -180;
constexpr int kRotationMax = 180;
} // namespace

OverlayInspectorPanel::OverlayInspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    // --- Empty state ------------------------------------------------------
    m_emptyState = new QWidget(this);
    auto* emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(16, 16, 16, 16);
    auto* emptyLabel = new QLabel(
        "Select a clip on an overlay track to animate its position, size and opacity.",
        m_emptyState);
    emptyLabel->setWordWrap(true);
    emptyLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    emptyLabel->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    emptyLayout->addWidget(emptyLabel);
    emptyLayout->addStretch();

    // --- Editor -----------------------------------------------------------
    m_editor = new QWidget(this);
    auto* editorLayout = new QVBoxLayout(m_editor);
    editorLayout->setContentsMargins(12, 12, 12, 12);
    editorLayout->setSpacing(10);

    // A thumbnail of the actual image beside its name. The panel is otherwise
    // five near-identical rows of sliders, and when several overlays are in play
    // a filename alone is a poor way to confirm you're editing the right one.
    m_thumbnail = new QLabel(m_editor);
    m_thumbnail->setFixedSize(56, 40);
    m_thumbnail->setAlignment(Qt::AlignCenter);
    m_thumbnail->setStyleSheet(
        QString("background: %1; border: 1px solid %2; border-radius: %3px;")
            .arg(Theme::bg0().name(), Theme::line().name()).arg(Theme::kRadiusSm));

    m_headerLabel = new QLabel(m_editor);
    m_headerLabel->setFont(Theme::uiFont(0, QFont::DemiBold));
    m_headerLabel->setWordWrap(true);

    m_timeLabel = new QLabel(m_editor);
    m_timeLabel->setFont(Theme::monoFont(-2));
    m_timeLabel->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    m_timeLabel->setWordWrap(true);

    auto* headerText = new QVBoxLayout();
    headerText->setSpacing(2);
    headerText->addWidget(m_headerLabel);
    headerText->addWidget(m_timeLabel);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(10);
    headerRow->addWidget(m_thumbnail);
    headerRow->addLayout(headerText, /*stretch=*/1);
    editorLayout->addLayout(headerRow);

    auto* headerRule = new QFrame(m_editor);
    headerRule->setFrameShape(QFrame::HLine);
    headerRule->setFixedHeight(1);
    headerRule->setStyleSheet(QString("background: %1; border: none;").arg(Theme::lineSoft().name()));
    editorLayout->addWidget(headerRule);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(9);
    editorLayout->addLayout(grid);

    auto addRow = [&](int row, Row& target, const QString& label, const QString& tip,
                      int min, int max, AnimatedProperty OverlayAnimation::*member,
                      const QString& suffix) {
        target = buildRow(label, tip, min, max, member, suffix);
        grid->addWidget(target.keyButton, row, 0);
        grid->addWidget(target.name, row, 1);
        grid->addWidget(target.slider, row, 2);
        grid->addWidget(target.value, row, 3);
    };

    auto sectionLabel = [&](const QString& text) {
        auto* label = new QLabel(text, m_editor);
        QFont font = Theme::uiFont(-2, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
        label->setFont(font);
        label->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
        return label;
    };

    // Grouped rather than one undifferentiated stack of five. Transform is what
    // you reach for constantly; opacity is a different kind of decision and gets
    // its own heading so the eye can skip straight to it.
    grid->addWidget(sectionLabel("TRANSFORM"), 0, 0, 1, 5);
    grid->addWidget(sectionLabel("APPEARANCE"), 5, 0, 1, 5);

    addRow(1, m_xRow, "X", "Horizontal position of the overlay's centre, as a percentage across the frame",
           kPosMin, kPosMax, &OverlayAnimation::x, "%");
    addRow(2, m_yRow, "Y", "Vertical position of the overlay's centre, as a percentage down the frame",
           kPosMin, kPosMax, &OverlayAnimation::y, "%");
    addRow(3, m_scaleRow, "Size", "Overlay width as a percentage of the frame width",
           kScaleMin, kScaleMax, &OverlayAnimation::scale, "%");
    addRow(4, m_rotationRow, "Rotation", "Rotation in degrees, clockwise about the overlay's centre",
           kRotationMin, kRotationMax, &OverlayAnimation::rotation, "\u00B0");
    addRow(6, m_opacityRow, "Opacity", "How opaque the overlay is",
           kOpacityMin, kOpacityMax, &OverlayAnimation::opacity, "%");

    grid->setColumnStretch(2, 1);

    auto* hint = new QLabel(
        "Drag the image on the preview to move it, corners to resize, the handle "
        "above it to rotate. Turn on a diamond to animate that property \u2014 each "
        "change then writes a keyframe at the playhead.",
        m_editor);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(Theme::textFaint().name()));
    editorLayout->addWidget(hint);
    editorLayout->addStretch();

    // The editor scrolls. Without this the panel's minimum height was whatever
    // its content happened to need, so the dock physically could not be dragged
    // any shorter — which made it impossible to give the timeline more room
    // while keeping the overlay controls open.
    m_editorScroll = new QScrollArea();
    m_editorScroll->setWidget(m_editor);
    m_editorScroll->setWidgetResizable(true);
    m_editorScroll->setFrameShape(QFrame::NoFrame);
    // No horizontal scrolling: the rows are laid out to fit the panel's width,
    // so a horizontal bar would only ever mean something has been sized wrong.
    m_editorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editorScroll->setMinimumHeight(60);

    // A stacked layout rather than show/hide juggling: exactly one of the two is
    // ever visible, which is precisely what a stack enforces for free.
    auto* stack = new QStackedLayout();
    stack->addWidget(m_emptyState);
    stack->addWidget(m_editorScroll);
    root->addLayout(stack);
    stack->setCurrentWidget(m_emptyState);

    // The panel itself must not impose a floor either, or the scroll area's
    // freedom to shrink would be academic.
    setMinimumHeight(60);
}

OverlayInspectorPanel::Row OverlayInspectorPanel::buildRow(
    const QString& label, const QString& tooltip, int min, int max,
    AnimatedProperty OverlayAnimation::*member, const QString& suffix) {
    Row row;

    // A real diamond icon rather than a text glyph: the glyphs rendered at
    // whatever size and weight the UI font happened to have, and "◆" vs "◇" is
    // a difference most fonts draw far too subtly to notice at 24px.
    row.keyButton = new QToolButton(this);
    row.keyButton->setObjectName("keyframeButton"); // see the matching rule in Theme.cpp
    row.keyButton->setCheckable(true);
    row.keyButton->setIconSize(QSize(16, 16));
    row.keyButton->setFixedSize(26, 26);
    row.keyButton->setCursor(Qt::PointingHandCursor);
    connect(row.keyButton, &QToolButton::clicked, this, [this, member] { toggleKeyframe(member); });

    row.name = new QLabel(label, this);
    row.name->setMinimumWidth(48);

    row.slider = new QSlider(Qt::Horizontal, this);
    row.slider->setRange(min, max);
    row.slider->setToolTip(tooltip);
    row.slider->setCursor(Qt::PointingHandCursor);
    connect(row.slider, &QSlider::valueChanged, this, [this, member](int v) {
        if (m_updating) return; // programmatic refresh, not a user edit
        applyDisplayValue(member, v);
    });

    // Editable rather than a read-only readout. Sliders are fine for finding a
    // value by eye but hopeless for "exactly 50%" or "exactly -15 degrees",
    // which is most of what you actually want when matching two overlays or
    // squaring something up.
    row.value = new QSpinBox(this);
    row.value->setRange(min, max);
    row.value->setSuffix(suffix);
    row.value->setFont(Theme::monoFont(-1));
    row.value->setFixedWidth(74);
    row.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row.value->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    row.value->setKeyboardTracking(false); // commit on Enter or focus-out, not per digit typed
    row.value->setToolTip(tooltip);
    connect(row.value, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, member](int v) {
        if (m_updating) return;
        applyDisplayValue(member, v);
    });

    // There is deliberately no per-row "clear" button here any more.
    //
    // It only existed while a property was animated, so it appeared and vanished
    // as you worked — shifting the row's layout under the pointer — and it put a
    // destructive, irreversible action (this app has no undo) one stray click
    // away from the slider you were reaching for. Removing an animation is both
    // rarer and more consequential than setting a keyframe, so it now lives in
    // the row's right-click menu, alongside the other keyframe navigation
    // actions that were previously unreachable.
    return row;
}

Clip* OverlayInspectorPanel::targetClip() const {
    if (!m_project) return nullptr;
    if (m_trackIndex < 0 || m_trackIndex >= m_project->tracks.size()) return nullptr;
    Track& track = m_project->tracks[m_trackIndex];
    if (track.type != TrackType::Overlay) return nullptr;
    if (m_clipIndex < 0 || m_clipIndex >= track.clips.size()) return nullptr;
    return &track.clips[m_clipIndex];
}

double OverlayInspectorPanel::localSec() const {
    const Clip* clip = targetClip();
    if (!clip) return 0.0;
    return m_playheadSec - clip->trackPosSec;
}

bool OverlayInspectorPanel::playheadInsideClip() const {
    const Clip* clip = targetClip();
    if (!clip) return false;
    const double t = localSec();
    return t >= -0.001 && t <= clip->durationSec() + 0.001;
}

void OverlayInspectorPanel::setTarget(Project* project, int trackIndex, int clipIndex) {
    m_project = project;
    m_trackIndex = trackIndex;
    m_clipIndex = clipIndex;
    refreshFromClip();
}

void OverlayInspectorPanel::setPlayheadSec(double timelineSec) {
    m_playheadSec = timelineSec;
    // Cheap enough to do on every tick, and it's what makes the sliders track an
    // animation as it plays rather than sitting frozen at whatever was last set.
    if (targetClip()) refreshFromClip();
}

void OverlayInspectorPanel::updateKeyButton(Row& row, const AnimatedProperty& prop, double t) {
    const bool animated = !prop.keys.isEmpty();
    const bool onKey = prop.hasKeyNear(t);

    row.keyButton->setChecked(animated);
    row.keyButton->setIcon(Theme::icon(onKey ? Theme::Icon::KeyframeFilled : Theme::Icon::Keyframe,
                                       onKey ? Theme::now()
                                             : (animated ? Theme::accent() : Theme::textFaint()),
                                       16));

    // Three distinct states, three distinct explanations — this control does
    // different things depending on which one it's in, so the tooltip has to say
    // which one that is rather than describing it generically.
    if (!animated) {
        row.keyButton->setToolTip("Not animated — this value applies for the whole clip.\n"
                                  "Click to start animating it from here.\n"
                                  "Right-click the row for more options.");
    } else if (onKey) {
        row.keyButton->setToolTip("Keyframe set at the playhead. Click to remove it.\n"
                                  "Right-click the row to navigate or remove the animation.");
    } else {
        row.keyButton->setToolTip("Animated. Click to add a keyframe at the playhead,\n"
                                  "or just change the value to set one.\n"
                                  "Right-click the row to navigate or remove the animation.");
    }
}

void OverlayInspectorPanel::refreshFromClip() {
    auto* stack = static_cast<QStackedLayout*>(layout()->itemAt(0)->layout());
    const Clip* clip = targetClip();

    if (!clip) {
        stack->setCurrentWidget(m_emptyState);
        return;
    }
    // The scroll area is the page, not the editor — the editor now lives inside
    // it, and asking a stack to show a widget that isn't one of its pages fails
    // silently, leaving the empty state up forever.
    stack->setCurrentWidget(m_editorScroll);

    const QString name = clip->sourcePath.section('/', -1).section('\\', -1);
    m_headerLabel->setText(name);

    const QImage source = OverlayImageLoader::load(clip->sourcePath);
    if (!source.isNull()) {
        m_thumbnail->setPixmap(QPixmap::fromImage(
            source.scaled(52, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    } else {
        m_thumbnail->setText("?");
    }

    const double t = localSec();
    m_timeLabel->setText(playheadInsideClip()
        ? QString("Playhead at %1s in this clip").arg(QString::number(std::max(0.0, t), 'f', 2))
        : QString("Playhead is outside this clip — move it inside to set keyframes"));

    m_updating = true;
    auto sync = [&](Row& row, AnimatedProperty OverlayAnimation::*member, double displayScale) {
        const AnimatedProperty& prop = clip->anim.*member;
        const int shown = static_cast<int>(std::lround(prop.valueAt(t) * displayScale));
        row.slider->setValue(std::clamp(shown, row.slider->minimum(), row.slider->maximum()));
        row.value->setValue(std::clamp(shown, row.value->minimum(), row.value->maximum()));

        updateKeyButton(row, prop, t);

        // Editing outside the clip's own span would write a keyframe at a
        // negative time or past the end, where it could never be reached.
        const bool editable = playheadInsideClip() || prop.keys.isEmpty();
        row.slider->setEnabled(editable);
        row.value->setEnabled(editable);
    };

    sync(m_xRow, &OverlayAnimation::x, 100.0);
    sync(m_yRow, &OverlayAnimation::y, 100.0);
    sync(m_scaleRow, &OverlayAnimation::scale, 100.0);
    // Degrees are stored as degrees, so unlike the others this one is 1:1.
    sync(m_rotationRow, &OverlayAnimation::rotation, 1.0);
    sync(m_opacityRow, &OverlayAnimation::opacity, 100.0);
    m_updating = false;
}

void OverlayInspectorPanel::applyDisplayValue(AnimatedProperty OverlayAnimation::*member, int displayValue) {
    Clip* clip = targetClip();
    if (!clip) return;

    AnimatedProperty& prop = clip->anim.*member;
    // Rotation is stored in degrees and shown in degrees; everything else is a
    // 0..1 fraction shown as a percentage.
    const bool isRotation = (member == &OverlayAnimation::rotation);
    const double value = isRotation ? displayValue : displayValue / 100.0;

    if (!prop.keys.isEmpty() && !playheadInsideClip()) return;

    // Shared with dragging the overlay directly on the preview, so the two can't
    // disagree about what setting a value means.
    OverlayAnimation::applyValue(prop, value, std::max(0.0, localSec()), playheadInsideClip());

    refreshFromClip();
    emit animationChanged();
}

void OverlayInspectorPanel::toggleKeyframe(AnimatedProperty OverlayAnimation::*member) {
    Clip* clip = targetClip();
    if (!clip) { refreshFromClip(); return; }

    AnimatedProperty& prop = clip->anim.*member;
    const double t = std::max(0.0, localSec());

    if (prop.hasKeyNear(t)) {
        prop.removeKeyNear(t);
        // Dropping to a single remaining key would leave a "constant" animation
        // that behaves identically to a static value but reads as animated in
        // the UI, so collapse it back to static and keep the value it had.
        if (prop.keys.size() == 1) {
            prop.staticValue = prop.keys.first().value;
            prop.keys.clear();
        }
    } else if (playheadInsideClip()) {
        // Seeding the first key from the CURRENT value means turning on
        // animation never makes the overlay jump.
        prop.setKey(t, prop.valueAt(t));
    }

    refreshFromClip();
    emit animationChanged();
}

OverlayInspectorPanel::Row* OverlayInspectorPanel::rowAt(const QPoint& pos) {
    // Matched on the row's vertical band rather than on which exact child widget
    // was hit, so right-clicking the gap between the slider and the number box
    // still finds the row you obviously meant.
    Row* rows[] = { &m_xRow, &m_yRow, &m_scaleRow, &m_rotationRow, &m_opacityRow };
    for (Row* row : rows) {
        if (!row->slider) continue;
        const QPoint local = row->slider->mapFrom(this, pos);
        if (local.y() >= -6 && local.y() <= row->slider->height() + 6) return row;
    }
    return nullptr;
}

AnimatedProperty OverlayAnimation::*OverlayInspectorPanel::memberForRow(const Row* row) const {
    if (row == &m_xRow) return &OverlayAnimation::x;
    if (row == &m_yRow) return &OverlayAnimation::y;
    if (row == &m_scaleRow) return &OverlayAnimation::scale;
    if (row == &m_rotationRow) return &OverlayAnimation::rotation;
    return &OverlayAnimation::opacity;
}

void OverlayInspectorPanel::contextMenuEvent(QContextMenuEvent* event) {
    Clip* clip = targetClip();
    Row* row = rowAt(event->pos());
    if (!clip || !row) { QWidget::contextMenuEvent(event); return; }

    auto member = memberForRow(row);
    AnimatedProperty& prop = clip->anim.*member;
    const double t = std::max(0.0, localSec());

    QMenu menu(this);

    // Jumping between keys had no UI at all before — you had to eyeball the
    // diamonds on the timeline clip and scrub onto them by hand.
    QAction* prevAction = menu.addAction("Go to previous keyframe");
    QAction* nextAction = menu.addAction("Go to next keyframe");
    prevAction->setEnabled(false);
    nextAction->setEnabled(false);

    double prevTime = -1.0, nextTime = -1.0;
    for (const auto& key : prop.keys) {
        if (key.timeSec < t - 0.01 && key.timeSec > prevTime) prevTime = key.timeSec;
        if (key.timeSec > t + 0.01 && (nextTime < 0.0 || key.timeSec < nextTime)) nextTime = key.timeSec;
    }
    prevAction->setEnabled(prevTime >= 0.0);
    nextAction->setEnabled(nextTime >= 0.0);

    menu.addSeparator();

    const bool onKey = prop.hasKeyNear(t);
    QAction* keyAction = menu.addAction(onKey ? "Remove keyframe here" : "Add keyframe here");
    keyAction->setEnabled(playheadInsideClip() || !prop.keys.isEmpty());

    menu.addSeparator();
    QAction* clearAction = menu.addAction(
        QString("Remove animation (%1 keyframe%2)")
            .arg(prop.keys.size()).arg(prop.keys.size() == 1 ? "" : "s"));
    clearAction->setEnabled(!prop.keys.isEmpty());

    QAction* chosen = menu.exec(event->globalPos());
    if (!chosen) return;

    if (chosen == clearAction) {
        clearProperty(member);
    } else if (chosen == keyAction) {
        toggleKeyframe(member);
    } else if (chosen == prevAction || chosen == nextAction) {
        const double target = (chosen == prevAction) ? prevTime : nextTime;
        // Emitted as a seek request rather than moved directly: the playhead
        // belongs to MainWindow, which has to bring the player and every audio
        // track along with it.
        emit seekRequested(clip->trackPosSec + target);
    }
}

void OverlayInspectorPanel::clearProperty(AnimatedProperty OverlayAnimation::*member) {
    Clip* clip = targetClip();
    if (!clip) return;

    AnimatedProperty& prop = clip->anim.*member;
    if (prop.keys.isEmpty()) return;

    // Keep whatever the property is worth right now, so clearing the animation
    // freezes the overlay where you're looking at it rather than snapping it
    // back to a default you never chose.
    prop.staticValue = prop.valueAt(std::max(0.0, localSec()));
    prop.keys.clear();

    refreshFromClip();
    emit animationChanged();
}
