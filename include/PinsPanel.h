#pragma once

#include <QWidget>
#include <QVector>
#include "Project.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QToolButton;
class QLabel;

// Lists the project's pins in time order, with their labels.
//
// Click a row to jump the playhead there; double-click the label (or press F2)
// to rename it; Delete removes it. The panel never touches the Project itself —
// every change is a request MainWindow carries out, so renames and deletes go
// through the same undo history as any other edit.
//
// Only PINS are listed (Project::markers also holds range markers, which have
// no way to be created yet). Rows are keyed by index into Project::markers, so
// the panel must be refreshed with setPins after anything that changes that
// vector — MainWindow does this after every recorded edit.
class PinsPanel : public QWidget {
    Q_OBJECT
public:
    explicit PinsPanel(QWidget* parent = nullptr);

    // Rebuilds the list. Keeps the selection on the same pin where it still
    // exists, and never fires rename requests while doing so.
    void setPins(const QVector<Marker>& markers);

    // Selects the pin nearest `sec` and opens its label for typing — used right
    // after adding a pin from this panel, so naming it is one continuous action.
    void beginRenameAt(double sec);

signals:
    void jumpRequested(double sec);
    void renameRequested(int markerIndex, const QString& label);
    void deleteRequested(int markerIndex);
    void addRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyFilter();
    void updateButtons();
    int markerIndexOf(const QTreeWidgetItem* item) const;

    QLineEdit* m_filter = nullptr;
    QTreeWidget* m_tree = nullptr;
    QToolButton* m_addButton = nullptr;
    QToolButton* m_deleteButton = nullptr;
    QLabel* m_emptyHint = nullptr;
    bool m_rebuilding = false;
};
