#include <QApplication>
#include "MainWindow.h"
#include "Theme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Identify the app before any QSettings-backed state is touched, so the
    // saved window layout lands somewhere predictable rather than under a
    // generic per-binary key.
    QApplication::setApplicationName("Video Editor");
    QApplication::setOrganizationName("VideoEditorProject");

    // Installs the dark palette, the Fusion style, the app font and the global
    // stylesheet. This has to happen before any widget is constructed —
    // widgets snapshot the palette at construction time, so a theme applied
    // afterwards leaves anything already built looking like the old default.
    Theme::apply(app);

    MainWindow window;
    window.show();

    return app.exec();
}
