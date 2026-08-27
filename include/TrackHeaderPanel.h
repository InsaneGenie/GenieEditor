#pragma once

#include <QWidget>
#include "Project.h"

class QContextMenuEvent;
class QLineEdit;

// The fixed track-header column: name, type indicator, mute toggle, and a
// drag handle on each track's bottom edge for resizing — kept as its own
// widget (rather than drawn inside Timeline) so it can sit outside
// Timeline's horizontal-scrolling QScrollArea entirely, matching Premiere
// Pro's frozen track-header column, which never scrolls sideways with the
// clips.
//
// Vertical scrolling (when there are more tracks than fit on screen) is
// synced externally: MainWindow connects this widget's containing
// QScrollArea's vertical scrollbar to Timeline's, so both move together.
class TrackHeaderPanel : public QWidget {
    Q_OBJECT
public:
    explicit TrackHeaderPanel(QWidget* parent = nullptr);

    void setProject(Project* project);
    QSize sizeHint() const override;

signals:
    // Emitted after this panel already flipped Track::muted directly —
    // listeners just need to know a repaint of clip rendering may be due
    // (e.g. Timeline dimming a muted track's clips).
    void muteToggled(int trackIndex);

    // Emitted after this panel already flipped Track::enabled directly —
    // listeners should re-sync playback immediately, since the track's
    // inclusion in preview changed (see the field comment on Track::enabled).
    void trackEnabledChanged(int trackIndex);

    // Fired when the delete button is clicked — deliberately does NOT
    // remove the track itself here. Deletion has ripple effects across
    // several other pieces of state (AudioPlayer instances, active overlay
    // tracking, current selection, currently-loaded playback path) that
    // only MainWindow has full context to fix up correctly, so it owns the
    // actual removal via Project::removeTrack.
    void deleteTrackRequested(int trackIndex);

    // Emitted continuously while dragging a resize handle, after
    // Track::heightPx has already been updated directly — Timeline needs to
    // recompute its own geometry/repaint since track heights are shared
    // layout state between the two widgets.
    void trackHeightChanged();

    // Emitted continuously while dragging the volume slider in the
    // right-click menu, after Track::volumePercent has already been
    // updated directly — MainWindow applies it to that track's AudioPlayer.
    void trackVolumeChanged(int trackIndex);

    // Emitted after a double-click rename is committed, after Track::name
    // has already been updated directly. Nothing currently needs to react
    // beyond this panel's own repaint (already handled internally), but
    // kept for consistency with the other track-property-changed signals.
    void trackRenamed(int trackIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    // Which of a track header's inline buttons a point falls on. Hit-testing and
    // hover feedback both route through this, so a button can never light up in
    // one place and act in another.
    enum class HeaderButton { None, Mute, Disable, Delete };
    HeaderButton buttonAt(const QPoint& pos, int trackIndex) const;

    QRect muteButtonRect(int trackIndex) const;
    QRect disableButtonRect(int trackIndex) const;
    QRect deleteButtonRect(int trackIndex) const;
    QRect nameLabelRect(int trackIndex) const;
    int trackIndexAt(int y) const;
    bool isNearBottomEdge(const QPoint& pos, int trackIndex) const;
    void startRenaming(int trackIndex);
    void finishRenaming();

    Project* m_project = nullptr;

    int m_resizingTrackIndex = -1;
    int m_resizeStartY = 0;
    int m_resizeStartHeight = 0;

    // Inline rename editor — created on first use, repositioned/shown over
    // whichever track's name is being renamed, hidden the rest of the time.
    QLineEdit* m_renameEdit = nullptr;
    int m_renamingTrackIndex = -1;

    // Hover feedback for the inline buttons. Without this they were flat
    // squares that gave no indication they were clickable at all until you'd
    // already clicked one.
    HeaderButton m_hoverButton = HeaderButton::None;
    int m_hoverButtonTrack = -1;

    static constexpr int kResizeHandlePx = 4;
};
