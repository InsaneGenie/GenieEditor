#pragma once

#include <QString>
#include <QVector>
#include "Project.h"

// Undo/redo for the whole project, by snapshot.
//
// --- Why snapshots rather than commands --------------------------------------
//
// The usual advice is to record each edit as a reversible command. That is the
// right call when state is large or shared, and the wrong one here: the project
// is a plain struct of QVectors, and QVector is implicitly shared. Copying a
// Project therefore copies a handful of pointers and bumps some refcounts --
// the clips' waveform peaks and thumbnail strips, which are the only large
// members, are never duplicated unless something writes to them.
//
// The alternative would mean writing, and keeping correct, an inverse for every
// operation in the editor: move, trim, split, delete, paste, speed, track add
// and remove, overlay keyframes. Each one is a chance for undo to restore
// something subtly wrong, and those bugs are miserable to find because they
// only appear several steps later. A snapshot cannot be subtly wrong -- it
// either restores the state or it doesn't.
//
// --- What it does NOT cover --------------------------------------------------
//
// View state: playhead position, zoom, selection, scroll. Undo restores what
// the project IS, not where you were looking. Restoring the playhead too would
// mean an undo that scrolls the timeline out from under you, which is more
// disorienting than helpful.
class UndoStack {
public:
    // Records a state. `label` describes the edit that PRODUCED it, and is what
    // the menu shows ("Undo Move Clip").
    //
    // Anything ahead of the current position is discarded first: once you undo
    // and then make a different edit, the old future is unreachable and keeping
    // it would let redo jump into a state that never followed from here.
    void record(const Project& project, const QString& label);

    // Seeds the initial state without creating an undo step. Called when a
    // project is opened or created, so the first edit has something to go back
    // to but there is nothing to undo before it.
    void reset(const Project& project);

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index >= 0 && m_index + 1 < m_states.size(); }

    // Descriptions for the menu items, empty when the action is unavailable.
    QString undoLabel() const;
    QString redoLabel() const;

    // Move one step and return the project state to restore. Callers must check
    // canUndo/canRedo first; these assert nothing and return an empty project
    // if called regardless.
    Project undo();
    Project redo();

    void clear();

    // Bounded so a long session can't grow without limit. Old states are
    // dropped from the front, which is why this is a depth rather than a byte
    // budget -- a snapshot's cost is dominated by shared data whose size is
    // hard to attribute to any one state.
    static constexpr int kMaxDepth = 100;

private:
    struct State {
        Project project;
        QString label; // the edit that produced this state
    };

    QVector<State> m_states;
    int m_index = -1; // into m_states; -1 when empty
};