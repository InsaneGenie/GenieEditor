#include <QApplication>
#include "MainWindow.h"
#include "Theme.h"
#include <QSslSocket>
#include <QSettings>
#include <QDebug>

namespace {

// Copies settings written under the app's previous names into the current one.
//
// There were TWO legacy stores, not one, and that was a bug rather than a
// design: MainWindow constructed QSettings("VideoEditorProject", "VideoEditor")
// explicitly while every other panel used the default constructor, which
// resolved to "VideoEditorProject" / "Video Editor" from the application name.
// The window layout and recent-projects list therefore lived in a different
// file from the API key and pinned folders. Both are folded into the single
// store the app now uses.
//
// Runs at most once: it bails as soon as the current store has anything in it,
// so a later launch can't overwrite newer settings with stale ones.
void migrateLegacySettings() {
    QSettings current;
    if (!current.allKeys().isEmpty()) return; // already migrated, or already in use

    const QVector<QPair<QString, QString>> legacyStores = {
        {"VideoEditorProject", "VideoEditor"},   // layout, recent projects, last project
        {"VideoEditorProject", "Video Editor"},  // API keys, places, volumes, model path
    };

    bool migratedAnything = false;
    for (const auto& store : legacyStores) {
        QSettings legacy(store.first, store.second);
        for (const QString& key : legacy.allKeys()) {
            // First writer wins, so if both stores somehow hold the same key
            // the earlier one above takes precedence.
            if (current.contains(key)) continue;
            current.setValue(key, legacy.value(key));
            migratedAnything = true;
        }
    }

    if (migratedAnything) {
        current.sync();
        qInfo() << "Migrated settings from the previous application name.";
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Identify the app before any QSettings-backed state is touched, so the
    // saved window layout lands somewhere predictable rather than under a
    // generic per-binary key.
    QApplication::setApplicationName("GenieEditor");
    QApplication::setOrganizationName("GenieEditor");

    // Renaming the app moves where Qt keeps its settings, so everything saved
    // under the old name — window layout, the Klipy API key, pinned folders,
    // the whisper model path, recent projects — would otherwise silently
    // vanish. Copied across once, before any widget is built and therefore
    // before anything reads a setting.
    migrateLegacySettings();

    // Installs the dark palette, the Fusion style, the app font and the global
    // stylesheet. This has to happen before any widget is constructed —
    // widgets snapshot the palette at construction time, so a theme applied
    // afterwards leaves anything already built looking like the old default.
    Theme::apply(app);
    
    qDebug() << "supportsSsl :" << QSslSocket::supportsSsl();
    qDebug() << "build ver   :" << QSslSocket::sslLibraryBuildVersionString();
    qDebug() << "runtime ver :" << QSslSocket::sslLibraryVersionString();
    qDebug() << "exe dir     :" << QCoreApplication::applicationDirPath();
    MainWindow window;
    window.show();

    return app.exec();
}
