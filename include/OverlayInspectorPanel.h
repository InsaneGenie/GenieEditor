#pragma once

#include <QWidget>
#include "Project.h"

class QSlider;
class QLabel;
class QToolButton;
class QPushButton;
class QSpinBox;
class QScrollArea;

// Property editor for the selected Overlay-track clip: position, scale and
// opacity, each of which can be either a fixed value or animated with
// keyframes.
//
// The interaction model is the one every NLE and animation tool uses, because
// it's the one that avoids a mode switch: the keyframe button on a row decides
// whether that property is animated. With it OFF, moving the slider changes a
// single fixed value. With it ON, moving the slider writes a keyframe AT THE
// PLAYHEAD — so "animate this" is just "go to a time, set a value, go to
// another time, set another value".
//
// This panel edits the Project in place and then emits animationChanged(), the
// same act-then-announce pattern the timeline widgets already use.
class OverlayInspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit OverlayInspectorPanel(QWidget* parent = nullptr);

    // Points the panel at a clip, or clears it when trackIndex/clipIndex don't
    // name an overlay clip. Anything else (a video clip, no selection) shows the
    // empty state rather than silently editing the wrong thing.
    void setTarget(Project* project, int trackIndex, int clipIndex);

    // The panel needs the playhead for two reasons: to know where to write a
    // keyframe, and to show the animated values as they actually are right now
    // while the project plays back.
    void setPlayheadSec(double timelineSec);

    int targetTrackIndex() const { return m_trackIndex; }
    int targetClipIndex() const { return m_clipIndex; }

signals:
    // The clip's animation was edited in place — re-composite the preview and
    // repaint the timeline.
    void animationChanged();

    // Asks for the playhead to move — used by the row menu's keyframe
    // navigation. This panel doesn't own the playhead and can't move it itself.
    void seekRequested(double timelineSec);

protected:
    // Right-click on a row is where the destructive and less-common keyframe
    // actions live — see the comment on the removed clear button in the .cpp.
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    // One editable property row: label, keyframe toggle, slider, value readout.
    struct Row {
        QLabel* name = nullptr;
        QToolButton* keyButton = nullptr;
        QSlider* slider = nullptr;
        QSpinBox* value = nullptr;
    };

    Row buildRow(const QString& label, const QString& tooltip, int min, int max,
                 AnimatedProperty OverlayAnimation::*member, const QString& suffix);

    // Applies a value that arrived in DISPLAY units (percent, or degrees) from
    // either the slider or the number box, converting once, here.
    void applyDisplayValue(AnimatedProperty OverlayAnimation::*member, int displayValue);

    // Repaints a row's keyframe button for the current state: not animated,
    // animated but between keys, or sitting exactly on a key.
    void updateKeyButton(Row& row, const AnimatedProperty& prop, double t);

    // Resolves the clip currently being edited, or nullptr if the selection is
    // stale — tracks and clips can be deleted while this panel still points at
    // them, so every access goes through here rather than trusting the indices.
    Clip* targetClip() const;

    void refreshFromClip();
    void toggleKeyframe(AnimatedProperty OverlayAnimation::*member);
    void clearProperty(AnimatedProperty OverlayAnimation::*member);

    // Which row (if any) a point in this panel falls on, for the right-click
    // menu. Returns nullptr when the click missed every row.
    Row* rowAt(const QPoint& pos);
    AnimatedProperty OverlayAnimation::*memberForRow(const Row* row) const;

    // Clip-relative time of the playhead, which is what keyframes are stored
    // against. Negative or past the end when the playhead is outside the clip.
    double localSec() const;
    bool playheadInsideClip() const;

    Project* m_project = nullptr;
    int m_trackIndex = -1;
    int m_clipIndex = -1;
    double m_playheadSec = 0.0;

    // Guards the sliders against re-entrancy: refreshFromClip() sets their
    // positions programmatically, and without this each of those writes would
    // come straight back in as a user edit and stamp a keyframe.
    bool m_updating = false;

    QWidget* m_emptyState = nullptr;
    QWidget* m_editor = nullptr;
    // The editor's scrolling container, and the page actually swapped into the
    // stack — the editor itself lives inside it, so it can't be the page.
    QScrollArea* m_editorScroll = nullptr;
    QLabel* m_thumbnail = nullptr;
    QLabel* m_headerLabel = nullptr;
    QLabel* m_timeLabel = nullptr;

    Row m_xRow, m_yRow, m_scaleRow, m_rotationRow, m_opacityRow;
};
