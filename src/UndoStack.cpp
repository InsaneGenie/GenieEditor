#include "UndoStack.h"

void UndoStack::reset(const Project& project) {
    m_states.clear();
    m_states.push_back({project, QString()});
    m_index = 0;
}

void UndoStack::record(const Project& project, const QString& label) {
    if (m_index < 0) {
        // Nothing seeded yet. Record the state as the baseline rather than
        // discarding it, so undo works even if reset() was never called.
        m_states.push_back({project, label});
        m_index = 0;
        return;
    }

    // Everything after the current position becomes unreachable the moment a
    // new edit happens: redo would otherwise jump into a state that no longer
    // follows from here.
    while (m_states.size() > m_index + 1) m_states.removeLast();

    m_states.push_back({project, label});
    m_index = m_states.size() - 1;

    // Drop from the front once too deep. The index moves with it, so the
    // current position still refers to the same state.
    while (m_states.size() > kMaxDepth) {
        m_states.removeFirst();
        --m_index;
    }
}

QString UndoStack::undoLabel() const {
    if (!canUndo()) return QString();
    // The label of the CURRENT state describes the edit that produced it, which
    // is precisely the edit undo is about to reverse.
    return m_states[m_index].label;
}

QString UndoStack::redoLabel() const {
    if (!canRedo()) return QString();
    return m_states[m_index + 1].label;
}

Project UndoStack::undo() {
    if (!canUndo()) return Project();
    --m_index;
    return m_states[m_index].project;
}

Project UndoStack::redo() {
    if (!canRedo()) return Project();
    ++m_index;
    return m_states[m_index].project;
}

void UndoStack::clear() {
    m_states.clear();
    m_index = -1;
}