#include "PinsPanel.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kMarkerIndexRole = Qt::UserRole;
constexpr int kTimeRole = Qt::UserRole + 1;
constexpr int kLabelRole = Qt::UserRole + 2; // the real label; display text may be a placeholder

enum Column { TimeColumn = 0, LabelColumn = 1 };

// H:MM:SS.t — tenths because pins are placed by hand and landing within a
// frame or two is what matters; hours always shown once the project has them,
// so rows line up in a long recording.
QString formatTime(double sec) {
    const qint64 tenths = static_cast<qint64>(std::llround(std::max(0.0, sec) * 10.0));
    const qint64 totalSec = tenths / 10;
    const int h = static_cast<int>(totalSec / 3600);
    const int m = static_cast<int>((totalSec / 60) % 60);
    const int s = static_cast<int>(totalSec % 60);
    const int t = static_cast<int>(tenths % 10);
    if (h > 0) {
        return QString("%1:%2:%3.%4").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')).arg(t);
    }
    return QString("%1:%2.%3").arg(m).arg(s, 2, 10, QChar('0')).arg(t);
}

// Unlabelled pins show a dim placeholder, but the editor must open on the REAL
// (empty) label — otherwise renaming would start from the placeholder text and
// a user who just presses Enter would save "Untitled pin" as a name.
class LabelDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // The app stylesheet gives every QLineEdit 6px/10px padding plus a border
    // — right for a form field, but it needs ~30px of height, and an inline
    // editor only gets the row's ~22px, so the text was drawn mostly outside
    // the box and clipped to a sliver. The editor gets its own compact style
    // instead, sized to the cell.
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (auto* line = qobject_cast<QLineEdit*>(editor)) {
            line->setStyleSheet(QString(
                "QLineEdit { padding: 0px 5px; margin: 0px; border: 1px solid %1;"
                " border-radius: 3px; background: %2; color: %3; }")
                .arg(Theme::accent().name(), Theme::bg0().name(), Theme::text().name()));
        }
        return editor;
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex&) const override {
        editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
    }

    // A little taller than the default so the editor has room to breathe
    // and the rows are easier to hit.
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), option.fontMetrics.height() + 12));
        return size;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (auto* line = qobject_cast<QLineEdit*>(editor)) {
            line->setText(index.data(kLabelRole).toString());
            line->setPlaceholderText("Name this pin");
            line->selectAll();
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }
};

} // namespace

PinsPanel::PinsPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    m_filter = new QLineEdit();
    m_filter->setPlaceholderText("Filter pins\u2026");
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    topRow->addWidget(m_filter, 1);

    m_addButton = new QToolButton();
    m_addButton->setText("+ Pin");
    m_addButton->setToolTip("Add a pin at the playhead and name it");
    m_addButton->setCursor(Qt::PointingHandCursor);
    connect(m_addButton, &QToolButton::clicked, this, &PinsPanel::addRequested);
    topRow->addWidget(m_addButton);

    m_deleteButton = new QToolButton();
    m_deleteButton->setIcon(Theme::icon(Theme::Icon::Trash, Theme::textDim()));
    m_deleteButton->setToolTip("Delete the selected pin  (Delete)");
    m_deleteButton->setCursor(Qt::PointingHandCursor);
    connect(m_deleteButton, &QToolButton::clicked, this, [this] {
        const int index = markerIndexOf(m_tree->currentItem());
        if (index >= 0) emit deleteRequested(index);
    });
    topRow->addWidget(m_deleteButton);
    layout->addLayout(topRow);

    m_tree = new QTreeWidget();
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({"Time", "Label"});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    // Double-click or F2 on the label renames; a single click only jumps, so
    // browsing the list never drops you into an editor by accident.
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_tree->setItemDelegateForColumn(LabelColumn, new LabelDelegate(m_tree));
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(TimeColumn, QHeaderView::ResizeToContents);
    m_tree->installEventFilter(this);
    layout->addWidget(m_tree, 1);

    m_emptyHint = new QLabel("No pins yet.\nPress M on the timeline, or \u201c+ Pin\u201d above,\n"
                             "to mark the playhead's position.");
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(QString("color: %1;").arg(Theme::textDim().name()));
    layout->addWidget(m_emptyHint, 1);

    // Clicked rather than currentItemChanged: re-clicking the row you're
    // already on should jump back to it, which is exactly what you want after
    // the playhead has moved on.
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (item) emit jumpRequested(item->data(TimeColumn, kTimeRole).toDouble());
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item) emit jumpRequested(item->data(TimeColumn, kTimeRole).toDouble());
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });

    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_rebuilding || !item || column != LabelColumn) return;
        const QString oldLabel = item->data(LabelColumn, kLabelRole).toString();
        const QString newLabel = item->text(LabelColumn).trimmed();
        const int index = markerIndexOf(item);
        if (index < 0 || newLabel == oldLabel) {
            // Nothing changed (or an empty edit of an unlabelled pin): put the
            // placeholder back rather than leaving a blank cell.
            QSignalBlocker block(m_tree);
            item->setText(LabelColumn, oldLabel.isEmpty() ? QString("Untitled pin") : oldLabel);
            return;
        }
        // Queued: handling the rename rebuilds this list, and deleting the
        // item from inside its own itemChanged emission would pull it out
        // from under the view mid-signal.
        QMetaObject::invokeMethod(this, [this, index, newLabel] {
            emit renameRequested(index, newLabel);
        }, Qt::QueuedConnection);
    });

    setPins({});
}

int PinsPanel::markerIndexOf(const QTreeWidgetItem* item) const {
    return item ? item->data(TimeColumn, kMarkerIndexRole).toInt() : -1;
}

void PinsPanel::setPins(const QVector<Marker>& markers) {
    m_rebuilding = true;
    const QSignalBlocker block(m_tree);

    // Remember the selected pin by TIME, not by index: a delete or insert
    // elsewhere shifts indices, but the pin you were on is still at its time.
    const QTreeWidgetItem* current = m_tree->currentItem();
    const double selectedTime = current ? current->data(TimeColumn, kTimeRole).toDouble() : -1.0;

    QVector<int> order;
    for (int i = 0; i < markers.size(); ++i) {
        if (markers[i].isPin()) order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return markers[a].startSec < markers[b].startSec;
    });

    m_tree->clear();
    QTreeWidgetItem* reselect = nullptr;
    const QFont mono = Theme::monoFont(-1);
    QFont placeholderFont = m_tree->font();
    placeholderFont.setItalic(true);

    for (int markerIndex : order) {
        const Marker& pin = markers[markerIndex];
        auto* item = new QTreeWidgetItem();
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(TimeColumn, kMarkerIndexRole, markerIndex);
        item->setData(TimeColumn, kTimeRole, pin.startSec);
        item->setData(LabelColumn, kLabelRole, pin.label);

        item->setText(TimeColumn, formatTime(pin.startSec));
        item->setFont(TimeColumn, mono);
        item->setForeground(TimeColumn, pin.color);

        if (pin.label.isEmpty()) {
            item->setText(LabelColumn, "Untitled pin");
            item->setFont(LabelColumn, placeholderFont);
            item->setForeground(LabelColumn, Theme::textDim());
        } else {
            item->setText(LabelColumn, pin.label);
        }
        item->setToolTip(LabelColumn, pin.label.isEmpty() ? QString("Double-click to name this pin") : pin.label);

        m_tree->addTopLevelItem(item);
        if (selectedTime >= 0.0 && std::abs(pin.startSec - selectedTime) < 1e-6) reselect = item;
    }
    if (reselect) m_tree->setCurrentItem(reselect);

    const bool empty = order.isEmpty();
    m_tree->setVisible(!empty);
    m_emptyHint->setVisible(empty);
    m_rebuilding = false;

    applyFilter();
    updateButtons();
}

void PinsPanel::beginRenameAt(double sec) {
    QTreeWidgetItem* best = nullptr;
    double bestDist = 1e18;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        const double d = std::abs(item->data(TimeColumn, kTimeRole).toDouble() - sec);
        if (d < bestDist) { bestDist = d; best = item; }
    }
    if (!best) return;
    // A filter that hides the new pin would make the editor open on nothing.
    if (best->isHidden()) m_filter->clear();
    m_tree->setCurrentItem(best);
    m_tree->scrollToItem(best);
    m_tree->setFocus();
    m_tree->editItem(best, LabelColumn);
}

void PinsPanel::applyFilter() {
    const QString needle = m_filter->text().trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        const QString label = item->data(LabelColumn, kLabelRole).toString();
        // Matches the label or the timecode, so "1:02" finds pins in that minute.
        const bool match = needle.isEmpty()
            || label.contains(needle, Qt::CaseInsensitive)
            || item->text(TimeColumn).contains(needle);
        item->setHidden(!match);
    }
}

void PinsPanel::updateButtons() {
    m_deleteButton->setEnabled(m_tree->currentItem() != nullptr);
}

bool PinsPanel::eventFilter(QObject* watched, QEvent* event) {
    // While a label is being edited, keys go to the editor rather than the
    // tree, so Delete/Backspace inside the text never reach this.
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            const int index = markerIndexOf(m_tree->currentItem());
            if (index >= 0) emit deleteRequested(index);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
