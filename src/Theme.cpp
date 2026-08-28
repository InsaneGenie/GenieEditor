#include "Theme.h"
#include <QTransform>

#include <QApplication>
#include <QFontDatabase>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStringList>
#include <QStyleFactory>

namespace {

// The base UI point size, captured from whatever the platform's default font
// was before we replaced it — so the app still respects a user who runs their
// desktop at a larger text size instead of hardcoding 9pt and looking wrong.
double g_basePointSize = 9.0;

QString firstAvailableFamily(const QStringList& candidates, const QString& fallback) {
    for (const QString& family : candidates) {
        if (QFontDatabase::hasFamily(family)) return family;
    }
    return fallback;
}

// -----------------------------------------------------------------------------
// Icon drawing
// -----------------------------------------------------------------------------
// Every icon is authored on a 24x24 grid with a 2px stroke, round caps and round
// joins, then scaled to whatever size is asked for. Keeping one grid and one
// stroke weight across the whole set is what makes them look like a family
// rather than a pile of clip art.

constexpr double kGrid = 24.0;
constexpr double kStroke = 2.0;

void strokePath(QPainter& p, const QPainterPath& path, const QColor& c, double width = kStroke) {
    QPen pen(c, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void fillPath(QPainter& p, const QPainterPath& path, const QColor& c) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(path);
}

QPainterPath linePath(double x1, double y1, double x2, double y2) {
    QPainterPath path;
    path.moveTo(x1, y1);
    path.lineTo(x2, y2);
    return path;
}

QPainterPath trianglePath(double x1, double y1, double x2, double y2, double x3, double y3) {
    QPainterPath path;
    path.moveTo(x1, y1);
    path.lineTo(x2, y2);
    path.lineTo(x3, y3);
    path.closeSubpath();
    return path;
}

// The small "+" badge used by the three add-track icons, drawn in the top-right
// corner so the main glyph underneath still reads as its track type.
void drawPlusBadge(QPainter& p, const QColor& c) {
    strokePath(p, linePath(20, 2.5, 20, 7.5), c, 2.0);
    strokePath(p, linePath(17.5, 5, 22.5, 5), c, 2.0);
}

// Speaker cone shared by the volume icons.
QPainterPath speakerPath() {
    QPainterPath path;
    path.moveTo(4, 9.5);
    path.lineTo(7.5, 9.5);
    path.lineTo(12, 5);
    path.lineTo(12, 19);
    path.lineTo(7.5, 14.5);
    path.lineTo(4, 14.5);
    path.closeSubpath();
    return path;
}

void drawIcon(QPainter& p, Theme::Icon which, const QColor& c) {
    using I = Theme::Icon;
    switch (which) {

    case I::Import: {
        // Arrow descending into an open tray.
        QPainterPath tray;
        tray.moveTo(4, 15.5);
        tray.lineTo(4, 19);
        tray.lineTo(20, 19);
        tray.lineTo(20, 15.5);
        strokePath(p, tray, c);
        strokePath(p, linePath(12, 4, 12, 14.5), c);
        QPainterPath chevron;
        chevron.moveTo(7.5, 10);
        chevron.lineTo(12, 14.5);
        chevron.lineTo(16.5, 10);
        strokePath(p, chevron, c);
        break;
    }

    case I::Export: {
        QPainterPath tray;
        tray.moveTo(4, 15.5);
        tray.lineTo(4, 19);
        tray.lineTo(20, 19);
        tray.lineTo(20, 15.5);
        strokePath(p, tray, c);
        strokePath(p, linePath(12, 14.5, 12, 4), c);
        QPainterPath chevron;
        chevron.moveTo(7.5, 8.5);
        chevron.lineTo(12, 4);
        chevron.lineTo(16.5, 8.5);
        strokePath(p, chevron, c);
        break;
    }

    case I::Split: {
        // Scissors: two blades crossing above two finger loops.
        strokePath(p, linePath(17.5, 4, 8.5, 15.5), c);
        strokePath(p, linePath(6.5, 4, 15.5, 15.5), c);
        QPainterPath loops;
        loops.addEllipse(QPointF(6.5, 18.5), 2.6, 2.6);
        loops.addEllipse(QPointF(17.5, 18.5), 2.6, 2.6);
        strokePath(p, loops, c, 1.8);
        break;
    }

    case I::Transcribe: {
        // Microphone: capsule, pickup arc, stand.
        QPainterPath capsule;
        capsule.addRoundedRect(QRectF(9, 3, 6, 10.5), 3, 3);
        strokePath(p, capsule, c);
        QPainterPath arc;
        arc.arcMoveTo(QRectF(5.5, 6.5, 13, 13), 200);
        arc.arcTo(QRectF(5.5, 6.5, 13, 13), 200, 140);
        strokePath(p, arc, c);
        strokePath(p, linePath(12, 18.5, 12, 21), c);
        break;
    }

    case I::ZoomIn:
    case I::ZoomOut: {
        QPainterPath lens;
        lens.addEllipse(QPointF(10.5, 10.5), 6.5, 6.5);
        strokePath(p, lens, c);
        strokePath(p, linePath(15.4, 15.4, 20.5, 20.5), c, 2.4);
        strokePath(p, linePath(7.5, 10.5, 13.5, 10.5), c);
        if (which == I::ZoomIn) strokePath(p, linePath(10.5, 7.5, 10.5, 13.5), c);
        break;
    }

    case I::Undo:
    case I::Redo: {
        // A curved arrow doubling back on itself. Drawn as an arc plus a
        // two-stroke head rather than a filled glyph, so it matches the weight
        // of the stroked icons beside it in the toolbar.
        const bool forward = (which == I::Redo);

        QPainterPath arc;
        arc.moveTo(6, 13);
        // Sweeps over the top and down the far side; the redo variant is the
        // same shape mirrored about the icon's centre.
        arc.cubicTo(6, 6, 18, 6, 18, 13);
        if (forward) {
            QTransform mirror;
            mirror.translate(24, 0);
            mirror.scale(-1, 1);
            arc = mirror.map(arc);
        }
        strokePath(p, arc, c);

        // Arrowhead at the tail the arrow travels back towards.
        const double tipX = forward ? 18.0 : 6.0;
        const double dir = forward ? 1.0 : -1.0;
        QPainterPath head;
        head.moveTo(tipX - dir * 4.0, 10.0);
        head.lineTo(tipX, 13.5);
        head.lineTo(tipX - dir * 4.0, 16.0);
        strokePath(p, head, c);
        break;
    }

    case I::ZoomFit: {
        // Four corner brackets pushing outward — "fill the available width".
        QPainterPath tl; tl.moveTo(4, 9); tl.lineTo(4, 4); tl.lineTo(9, 4);
        QPainterPath tr; tr.moveTo(15, 4); tr.lineTo(20, 4); tr.lineTo(20, 9);
        QPainterPath br; br.moveTo(20, 15); br.lineTo(20, 20); br.lineTo(15, 20);
        QPainterPath bl; bl.moveTo(9, 20); bl.lineTo(4, 20); bl.lineTo(4, 15);
        strokePath(p, tl, c); strokePath(p, tr, c);
        strokePath(p, br, c); strokePath(p, bl, c);
        strokePath(p, linePath(8, 12, 16, 12), c, 1.6);
        break;
    }

    case I::AddVideo: {
        QPainterPath frame;
        frame.addRoundedRect(QRectF(2.5, 6, 15, 12), 2.5, 2.5);
        strokePath(p, frame, c, 1.8);
        fillPath(p, trianglePath(8, 9.5, 8, 14.5, 12.5, 12), c);
        drawPlusBadge(p, c);
        break;
    }

    case I::AddAudio: {
        // Waveform bars — the same visual language the audio clips use.
        strokePath(p, linePath(3.5, 10, 3.5, 14), c, 1.8);
        strokePath(p, linePath(7, 7, 7, 17), c, 1.8);
        strokePath(p, linePath(10.5, 9.5, 10.5, 14.5), c, 1.8);
        strokePath(p, linePath(14, 5.5, 14, 18.5), c, 1.8);
        strokePath(p, linePath(17.5, 10.5, 17.5, 13.5), c, 1.8);
        drawPlusBadge(p, c);
        break;
    }

    case I::AddOverlay: {
        // Two stacked planes — overlays composite on top of video.
        QPainterPath back;
        back.addRoundedRect(QRectF(2.5, 6, 10, 8), 2, 2);
        strokePath(p, back, c, 1.8);
        QPainterPath front;
        front.addRoundedRect(QRectF(7.5, 11, 10, 8), 2, 2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 0));
        strokePath(p, front, c, 1.8);
        drawPlusBadge(p, c);
        break;
    }

    case I::Play:
        fillPath(p, trianglePath(7.5, 4.5, 7.5, 19.5, 19.5, 12), c);
        break;

    case I::Pause: {
        QPainterPath bars;
        bars.addRoundedRect(QRectF(7, 4.5, 3.8, 15), 1.4, 1.4);
        bars.addRoundedRect(QRectF(13.2, 4.5, 3.8, 15), 1.4, 1.4);
        fillPath(p, bars, c);
        break;
    }

    case I::SkipBack:
        fillPath(p, trianglePath(11.5, 5, 11.5, 19, 3.5, 12), c);
        fillPath(p, trianglePath(20.5, 5, 20.5, 19, 12.5, 12), c);
        break;

    case I::SkipForward:
        fillPath(p, trianglePath(3.5, 5, 3.5, 19, 11.5, 12), c);
        fillPath(p, trianglePath(12.5, 5, 12.5, 19, 20.5, 12), c);
        break;

    case I::GoToStart: {
        QPainterPath bar;
        bar.addRoundedRect(QRectF(4, 5, 2.6, 14), 1.2, 1.2);
        fillPath(p, bar, c);
        fillPath(p, trianglePath(20, 5, 20, 19, 8.5, 12), c);
        break;
    }

    case I::GoToEnd: {
        QPainterPath bar;
        bar.addRoundedRect(QRectF(17.4, 5, 2.6, 14), 1.2, 1.2);
        fillPath(p, bar, c);
        fillPath(p, trianglePath(4, 5, 4, 19, 15.5, 12), c);
        break;
    }

    case I::Volume: {
        fillPath(p, speakerPath(), c);
        QPainterPath inner;
        inner.arcMoveTo(QRectF(11, 8, 7, 8), -60);
        inner.arcTo(QRectF(11, 8, 7, 8), -60, 120);
        strokePath(p, inner, c, 1.7);
        QPainterPath outer;
        outer.arcMoveTo(QRectF(11, 4.5, 11.5, 15), -55);
        outer.arcTo(QRectF(11, 4.5, 11.5, 15), -55, 110);
        strokePath(p, outer, c, 1.7);
        break;
    }

    case I::VolumeMute: {
        fillPath(p, speakerPath(), c);
        strokePath(p, linePath(15, 9.5, 21, 15.5), c, 1.9);
        strokePath(p, linePath(21, 9.5, 15, 15.5), c, 1.9);
        break;
    }

    case I::Search: {
        QPainterPath lens;
        lens.addEllipse(QPointF(10.5, 10.5), 6.0, 6.0);
        strokePath(p, lens, c, 1.9);
        strokePath(p, linePath(15, 15, 20, 20), c, 2.2);
        break;
    }

    case I::FolderUp: {
        strokePath(p, linePath(4.5, 4, 19.5, 4), c, 2.0);
        strokePath(p, linePath(12, 20, 12, 8), c);
        QPainterPath chevron;
        chevron.moveTo(6.5, 13.5);
        chevron.lineTo(12, 8);
        chevron.lineTo(17.5, 13.5);
        strokePath(p, chevron, c);
        break;
    }

    case I::Folder: {
        QPainterPath folder;
        folder.moveTo(3, 18.5);
        folder.lineTo(3, 5.5);
        folder.lineTo(9.5, 5.5);
        folder.lineTo(11.5, 8.5);
        folder.lineTo(21, 8.5);
        folder.lineTo(21, 18.5);
        folder.closeSubpath();
        strokePath(p, folder, c, 1.9);
        break;
    }

    case I::Trash: {
        strokePath(p, linePath(4, 6.5, 20, 6.5), c, 1.9);
        strokePath(p, linePath(9.5, 6.5, 10.2, 4), c, 1.7);
        strokePath(p, linePath(14.5, 6.5, 13.8, 4), c, 1.7);
        QPainterPath body;
        body.moveTo(6, 6.5);
        body.lineTo(7, 20);
        body.lineTo(17, 20);
        body.lineTo(18, 6.5);
        strokePath(p, body, c, 1.9);
        strokePath(p, linePath(10.3, 10, 10.6, 16.5), c, 1.5);
        strokePath(p, linePath(13.7, 10, 13.4, 16.5), c, 1.5);
        break;
    }

    case I::Eye: {
        QPainterPath eye;
        eye.moveTo(2.5, 12);
        eye.cubicTo(6.5, 5.5, 17.5, 5.5, 21.5, 12);
        eye.cubicTo(17.5, 18.5, 6.5, 18.5, 2.5, 12);
        strokePath(p, eye, c, 1.9);
        QPainterPath pupil;
        pupil.addEllipse(QPointF(12, 12), 2.6, 2.6);
        fillPath(p, pupil, c);
        break;
    }

    case I::EyeOff: {
        QPainterPath eye;
        eye.moveTo(2.5, 12);
        eye.cubicTo(6.5, 5.5, 17.5, 5.5, 21.5, 12);
        eye.cubicTo(17.5, 18.5, 6.5, 18.5, 2.5, 12);
        strokePath(p, eye, c, 1.7);
        strokePath(p, linePath(4.5, 19.5, 19.5, 4.5), c, 2.1);
        break;
    }

    case I::Pin: {
        // A map-pin style tag with a point, matching the flag shape the timeline
        // draws in its ruler — the button and the thing it creates look alike.
        QPainterPath flag;
        flag.moveTo(5, 5);
        flag.lineTo(19, 5);
        flag.lineTo(19, 14);
        flag.lineTo(12, 20);
        flag.lineTo(5, 14);
        flag.closeSubpath();
        strokePath(p, flag, c, 1.9);
        strokePath(p, linePath(12, 8.5, 12, 12.5), c, 1.7);
        break;
    }

    case I::Keyframe:
    case I::KeyframeFilled: {
        // The diamond is the universal keyframe glyph across editing tools, so
        // it needs no explanation to anyone who's animated anything before.
        // Hollow means the property is a fixed value; filled means there's a key
        // at this exact moment.
        QPainterPath diamond;
        diamond.moveTo(12, 3.5);
        diamond.lineTo(20.5, 12);
        diamond.lineTo(12, 20.5);
        diamond.lineTo(3.5, 12);
        diamond.closeSubpath();
        if (which == I::KeyframeFilled) fillPath(p, diamond, c);
        else strokePath(p, diamond, c, 2.0);
        break;
    }

    case I::PinFilled: {
        // Solid counterpart to Icon::Pin, for "this folder is already pinned".
        QPainterPath flag;
        flag.moveTo(5, 5);
        flag.lineTo(19, 5);
        flag.lineTo(19, 14);
        flag.lineTo(12, 20);
        flag.lineTo(5, 14);
        flag.closeSubpath();
        fillPath(p, flag, c);
        break;
    }

    case I::ChevronDown: {
        QPainterPath chevron;
        chevron.moveTo(6, 9.5);
        chevron.lineTo(12, 15.5);
        chevron.lineTo(18, 9.5);
        strokePath(p, chevron, c, 2.0);
        break;
    }

    case I::ChevronRight: {
        QPainterPath chevron;
        chevron.moveTo(9.5, 6);
        chevron.lineTo(15.5, 12);
        chevron.lineTo(9.5, 18);
        strokePath(p, chevron, c, 2.0);
        break;
    }

    case I::Clear: {
        strokePath(p, linePath(6.5, 6.5, 17.5, 17.5), c, 2.1);
        strokePath(p, linePath(17.5, 6.5, 6.5, 17.5), c, 2.1);
        break;
    }
    }
}

// -----------------------------------------------------------------------------
// Stylesheet
// -----------------------------------------------------------------------------
// Written with @token placeholders and resolved against the palette above, so
// there is exactly one place a colour is ever defined. Note that there is
// deliberately NO blanket `QWidget { background: ... }` rule: PlayerWidget hands
// a native window handle to mpv, and a global background rule on QWidget can
// fight mpv's own rendering into that surface. Base surfaces come from the
// QPalette instead (which mpv's native window is unaffected by), and the
// stylesheet only ever targets specific widget classes.

const char* kStyleSheetTemplate = R"QSS(
/* ---------- Chrome ---------------------------------------------------- */
QMainWindow {
    background: @bg1;
}
QMainWindow::separator {
    background: @bg0;
    width: 4px;
    height: 4px;
}
QMainWindow::separator:hover {
    background: @accentDim;
}

QStatusBar {
    background: @bg2;
    border-top: 1px solid @lineSoft;
    color: @textDim;
    min-height: 24px;
}
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @textDim; padding: 0 4px; }

/* ---------- Menus ------------------------------------------------------ */
QMenuBar {
    background: @bg2;
    border-bottom: 1px solid @lineSoft;
    padding: 2px 4px;
}
QMenuBar::item {
    background: transparent;
    padding: 5px 10px;
    border-radius: @radiusSm;
    color: @textDim;
}
QMenuBar::item:selected { background: @bg3; color: @text; }
QMenuBar::item:pressed  { background: @bg4; color: @text; }

QMenu {
    background: @bg2;
    border: 1px solid @line;
    border-radius: @radiusMd;
    padding: 5px;
    color: @text;
}
QMenu::item {
    padding: 6px 26px 6px 12px;
    border-radius: @radiusSm;
}
QMenu::item:selected { background: @accentDim; color: #FFFFFF; }
QMenu::item:disabled { color: @textFaint; }
QMenu::separator {
    height: 1px;
    background: @line;
    margin: 5px 8px;
}

/* ---------- Toolbar ---------------------------------------------------- */
QToolBar {
    background: @bg2;
    border: none;
    border-bottom: 1px solid @lineSoft;
    padding: 5px 6px;
    spacing: 3px;
}
QToolBar::separator {
    background: @line;
    width: 1px;
    margin: 5px 8px;
}
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: @radiusMd;
    padding: 5px 9px;
    color: @textDim;
    font-weight: 500;
}
QToolButton:hover {
    background: @bg3;
    border-color: @line;
    color: @text;
}
QToolButton:pressed { background: @bg4; }
QToolButton:checked {
    background: @accentWash;
    border-color: @accentDim;
    color: @accent;
}
QToolButton:disabled { color: @textFaint; }
QToolButton::menu-indicator { image: none; }

/* Square icon-only toggles (the inspector's keyframe diamonds). The general
   QToolButton padding above is sized for text-beside-icon toolbar buttons; on a
   fixed 26px square it eats the content rect and Qt shrinks the icon to fit
   whatever's left, which rendered a 16px diamond at about 8px. */
QToolButton#keyframeButton {
    padding: 0px;
    border-radius: @radiusSm;
}

/* The media browser's collapsible section header — a full-width label that
   happens to be clickable, so it gets none of the usual button chrome. */
QToolButton#placesHeader {
    background: transparent;
    border: none;
    color: @textFaint;
    padding: 3px 2px;
    text-align: left;
}
QToolButton#placesHeader:hover { color: @textDim; }
QToolButton#placesHeader:checked { background: transparent; border: none; color: @textFaint; }

/* ---------- Buttons ---------------------------------------------------- */
QPushButton {
    background-color: @bg3;
    border: 1px solid @line;
    border-radius: @radiusMd;
    padding: 6px 13px;
    color: @text;
    font-weight: 500;
}
QPushButton:hover   { background-color: @bg4; border-color: @lineHi; }
QPushButton:pressed { background-color: @bg2; }
QPushButton:disabled { background-color: @bg2; color: @textFaint; border-color: @lineSoft; }

/* Opt-in variants, set via setProperty("variant", "primary" | "danger" | "ghost"). */
QPushButton[variant="primary"] {
    background-color: @accent;
    border-color: @accent;
    color: #061417;
    font-weight: 600;
}
QPushButton[variant="primary"]:hover   { background-color: @accentHi; border-color: @accentHi; }
QPushButton[variant="primary"]:pressed { background-color: @accentDim; border-color: @accentDim; }

QPushButton[variant="danger"] {
    background-color: transparent;
    border-color: @dangerDim;
    color: @danger;
}
QPushButton[variant="danger"]:hover { background-color: @dangerWash; }

QPushButton[variant="ghost"] {
    background-color: transparent;
    border-color: transparent;
    color: @textDim;
}
QPushButton[variant="ghost"]:hover { background-color: @bg3; color: @text; }

/* ---------- Docks ------------------------------------------------------- */
QDockWidget {
    color: @textDim;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background: @bg2;
    padding: 7px 10px;
    border-bottom: 1px solid @lineSoft;
    text-align: left;
    font-weight: 600;
    letter-spacing: 0.7px;
}
QDockWidget::close-button, QDockWidget::float-button {
    background: transparent;
    border: none;
    border-radius: @radiusSm;
    padding: 0px;
    icon-size: 12px;
}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {
    background: @bg4;
}

/* ---------- Tabs -------------------------------------------------------- */
/* Underline tabs rather than the default raised "folder tab" look — flatter,
   and the active-state indicator reuses the accent hue used everywhere else. */
QTabWidget::pane {
    border: none;
    border-top: 1px solid @lineSoft;
    background: @bg1;
}
QTabBar {
    background: @bg2;
    qproperty-drawBase: 0;
}
QTabBar::tab {
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
    padding: 7px 14px;
    margin-right: 2px;
    color: @textDim;
    font-weight: 500;
}
QTabBar::tab:hover     { color: @text; background: @bg3; }
QTabBar::tab:selected  { color: @accent; border-bottom-color: @accent; background: transparent; }

/* ---------- Text input --------------------------------------------------- */
QLineEdit {
    background: @bg0;
    border: 1px solid @line;
    border-radius: @radiusMd;
    padding: 6px 10px;
    color: @text;
    selection-background-color: @accentDim;
    selection-color: #FFFFFF;
}
QLineEdit:focus { border-color: @accent; background: @bg1; }
QLineEdit:disabled { color: @textFaint; background: @bg1; }

/* ---------- Number entry -------------------------------------------------- */
QSpinBox, QDoubleSpinBox {
    background: @bg0;
    border: 1px solid @line;
    border-radius: @radiusMd;
    padding: 4px 6px;
    color: @text;
    selection-background-color: @accentDim;
    selection-color: #FFFFFF;
}
QSpinBox:focus, QDoubleSpinBox:focus { border-color: @accent; background: @bg1; }
QSpinBox:disabled, QDoubleSpinBox:disabled { color: @textFaint; background: @bg1; }
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {
    background: @bg3;
    border: none;
    border-left: 1px solid @line;
    width: 15px;
}
QSpinBox::up-button { border-top-right-radius: @radiusMd; }
QSpinBox::down-button { border-bottom-right-radius: @radiusMd; }
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: @bg4; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: none;
    width: 0; height: 0;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-bottom: 4px solid @textDim;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: none;
    width: 0; height: 0;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-top: 4px solid @textDim;
}

/* ---------- Lists / views ------------------------------------------------ */
QListView, QTreeView, QListWidget {
    background: @bg1;
    border: none;
    outline: none;
    color: @text;
    selection-background-color: transparent;
}
QListView::item, QTreeView::item, QListWidget::item {
    border-radius: @radiusSm;
    padding: 4px 6px;
    margin: 1px 3px;
}
QListView::item:hover, QListWidget::item:hover, QTreeView::item:hover {
    background: @bg3;
}
QListView::item:selected, QListWidget::item:selected, QTreeView::item:selected {
    background: @accentWash;
    color: @text;
}

QScrollArea { border: none; background: @bg1; }
QAbstractScrollArea { border: none; }

/* ---------- Scrollbars ---------------------------------------------------- */
/* Thin, trackless, no stepper arrows — the modern overlay-ish treatment.
   Timeline scrolling especially benefits: the bar stops competing visually
   with the clips directly above it. */
QScrollBar:vertical {
    background: transparent;
    width: 11px;
    margin: 0px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 11px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: @bg4;
    min-height: 30px;
    border-radius: 5px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: @bg4;
    min-width: 30px;
    border-radius: 5px;
    margin: 2px;
}
QScrollBar::handle:hover { background: @lineHi; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---------- Sliders -------------------------------------------------------- */
QSlider::groove:horizontal {
    background: @bg0;
    height: 4px;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: @accentDim;
    height: 4px;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: @text;
    border: none;
    width: 12px;
    height: 12px;
    margin: -4px 0;
    border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: @accent; }

/* ---------- Progress -------------------------------------------------------- */
/* Amber, because a running transcription is a time-based "in progress" state —
   the same role the playhead colour carries on the timeline. */
QProgressBar {
    background: @bg0;
    border: 1px solid @line;
    border-radius: 6px;
    height: 12px;
    text-align: center;
    color: @textDim;
    font-size: 10px;
}
QProgressBar::chunk {
    background: @now;
    border-radius: 5px;
    margin: 1px;
}

/* ---------- Tooltips --------------------------------------------------------- */
QToolTip {
    background: @bg4;
    border: 1px solid @line;
    border-radius: @radiusSm;
    color: @text;
    padding: 5px 8px;
}

/* ---------- Misc ------------------------------------------------------------- */
QSplitter::handle { background: @bg0; }
QSplitter::handle:hover { background: @accentDim; }
QLabel { color: @text; background: transparent; }
QLabel[variant="caption"] { color: @textDim; }
QLabel[variant="mono"] { color: @textDim; }
)QSS";

QString resolveTokens(QString sheet) {
    const QHash<QString, QString> tokens = {
        {"@bg0",        Theme::bg0().name()},
        {"@bg1",        Theme::bg1().name()},
        {"@bg2",        Theme::bg2().name()},
        {"@bg3",        Theme::bg3().name()},
        {"@bg4",        Theme::bg4().name()},
        {"@line",       Theme::line().name()},
        {"@lineSoft",   Theme::lineSoft().name()},
        {"@lineHi",     Theme::line().lighter(150).name()},
        {"@text",       Theme::text().name()},
        {"@textDim",    Theme::textDim().name()},
        {"@textFaint",  Theme::textFaint().name()},
        {"@accent",     Theme::accent().name()},
        {"@accentDim",  Theme::accentDim().name()},
        {"@accentHi",   Theme::accent().lighter(115).name()},
        // rgba() so the wash tints whatever surface is underneath rather than
        // baking in an assumption about which one that is.
        {"@accentWash", QString("rgba(%1, %2, %3, 0.16)")
                            .arg(Theme::accent().red()).arg(Theme::accent().green()).arg(Theme::accent().blue())},
        {"@now",        Theme::now().name()},
        {"@danger",     Theme::danger().name()},
        {"@dangerDim",  Theme::danger().darker(160).name()},
        {"@dangerWash", QString("rgba(%1, %2, %3, 0.15)")
                            .arg(Theme::danger().red()).arg(Theme::danger().green()).arg(Theme::danger().blue())},
        {"@radiusSm",   QString::number(Theme::kRadiusSm) + "px"},
        {"@radiusMd",   QString::number(Theme::kRadiusMd) + "px"},
        {"@radiusLg",   QString::number(Theme::kRadiusLg) + "px"},
    };

    // Longest-token-first so "@bg0" can never be matched as a prefix of a
    // longer token that happens to start the same way.
    QStringList keys = tokens.keys();
    std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) {
        return a.size() > b.size();
    });
    for (const QString& key : keys) {
        sheet.replace(key, tokens.value(key));
    }
    return sheet;
}

} // namespace

namespace Theme {

QFont uiFont(int pointSizeDelta, int weight) {
    static const QString family = firstAvailableFamily(
        {"Segoe UI Variable Text", "Segoe UI", "Inter", "SF Pro Text", "Ubuntu", "Noto Sans"},
        QApplication::font().family());
    QFont f(family);
    f.setPointSizeF(g_basePointSize + pointSizeDelta);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QFont monoFont(int pointSizeDelta, int weight) {
    static const QString family = firstAvailableFamily(
        {"Cascadia Mono", "JetBrains Mono", "Consolas", "SF Mono", "DejaVu Sans Mono", "Menlo", "Courier New"},
        QString());
    QFont f;
    if (family.isEmpty()) {
        f.setStyleHint(QFont::Monospace);
        f.setFamily("monospace");
    } else {
        f.setFamily(family);
    }
    f.setPointSizeF(g_basePointSize + pointSizeDelta);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QIcon icon(Icon which, const QColor& color, int sizePx) {
    const QColor c = color.isValid() ? color : text();

    // Rendered at 4x and tagged with a matching device pixel ratio, so the icon
    // stays sharp both on a HiDPI display and when Qt scales it down to fit a
    // smaller button than it was asked for.
    constexpr qreal kSupersample = 4.0;
    QPixmap pm(qRound(sizePx * kSupersample), qRound(sizePx * kSupersample));
    pm.setDevicePixelRatio(kSupersample);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(sizePx / kGrid, sizePx / kGrid);
    drawIcon(p, which, c);
    p.end();

    return QIcon(pm);
}

QString stylesheet() {
    return resolveTokens(QString::fromUtf8(kStyleSheetTemplate));
}

void apply(QApplication& app) {
    // Capture the platform's own default text size before overriding the font,
    // then clamp it — some Linux desktops default very large, and the timeline's
    // fixed-height ruler/track chrome has limited room to absorb that.
    g_basePointSize = qBound(8.5, app.font().pointSizeF() > 0 ? app.font().pointSizeF() : 9.0, 11.0);

    // Fusion is the only Qt style that applies a custom palette faithfully on
    // every platform. The native Windows style silently ignores most palette
    // roles, which is exactly why this app previously had dark custom widgets
    // sitting inside light native chrome.
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window,          bg1());
    pal.setColor(QPalette::WindowText,      text());
    pal.setColor(QPalette::Base,            bg0());
    pal.setColor(QPalette::AlternateBase,   bg2());
    pal.setColor(QPalette::ToolTipBase,     bg4());
    pal.setColor(QPalette::ToolTipText,     text());
    pal.setColor(QPalette::Text,            text());
    pal.setColor(QPalette::Button,          bg3());
    pal.setColor(QPalette::ButtonText,      text());
    pal.setColor(QPalette::BrightText,      Qt::white);
    pal.setColor(QPalette::Link,            accent());
    pal.setColor(QPalette::LinkVisited,     accent().darker(120));
    pal.setColor(QPalette::Highlight,       accentDim());
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, textFaint());
    pal.setColor(QPalette::Mid,             line());
    pal.setColor(QPalette::Dark,            bg0());
    pal.setColor(QPalette::Shadow,          QColor(0, 0, 0));

    pal.setColor(QPalette::Disabled, QPalette::WindowText, textFaint());
    pal.setColor(QPalette::Disabled, QPalette::Text,       textFaint());
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, textFaint());
    pal.setColor(QPalette::Disabled, QPalette::Highlight,  bg4());

    app.setPalette(pal);
    app.setFont(uiFont());
    app.setStyleSheet(stylesheet());
}

} // namespace Theme
