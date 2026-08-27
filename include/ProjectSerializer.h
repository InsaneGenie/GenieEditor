#pragma once

#include <QString>
#include "Project.h"

// Reads and writes .veproj project files.
//
// --- Format ------------------------------------------------------------------
//
// JSON, deliberately. A project file is small (it's an edit decision list, not
// media), and a format you can open in a text editor is the difference between
// a corrupt project being diagnosable and being a dead end. It also diffs and
// merges, which matters the moment a project lives in version control.
//
// --- What is NOT saved -------------------------------------------------------
//
// Waveform peaks and frame thumbnails are omitted on purpose, even though
// they're part of a Clip and reproducing them costs real time on load. They are
// DERIVED data — recomputable from the source file at any moment — and a single
// video clip's thumbnail strip is megabytes of uncompressed QImage. Writing
// them would turn a few-kilobyte project into a few hundred megabytes, make the
// file unreadable by anything else, and go stale the instant a source file was
// re-exported. They're regenerated in the background after loading instead, the
// same way they are on import.
//
// --- Paths -------------------------------------------------------------------
//
// Each clip stores its source BOTH as an absolute path and as one relative to
// the project file. Absolute alone breaks whenever a project moves between
// machines or drive letters; relative alone breaks whenever the project file is
// moved but the media isn't. Trying absolute first and falling back to relative
// covers both of the ways this actually goes wrong in practice.
class ProjectSerializer {
public:
    struct LoadResult {
        bool ok = false;
        QString error;             // human-readable, empty when ok
        QStringList missingMedia;  // sources that couldn't be found at either path
        double playheadSec = 0.0;
        double pixelsPerSecond = 0.0; // 0 when the file didn't record a zoom level
    };

    // Writes `project` to `path`. `playheadSec` and `pixelsPerSecond` are view
    // state rather than project data, but they're what makes reopening feel
    // like returning to the same session rather than to the same clips.
    //
    // The write goes to a temporary file which then replaces the target, so an
    // interrupted save can't destroy the previous version — losing an edit is
    // bad, losing the whole project because the power went out mid-write is
    // unrecoverable.
    static bool save(const Project& project, const QString& path,
                     double playheadSec, double pixelsPerSecond, QString* error = nullptr);

    // Replaces `project` wholesale with the file's contents. On failure
    // `project` is left untouched, so a bad file can't half-destroy what's
    // currently open.
    static LoadResult load(Project& project, const QString& path);

    // Bumped only when a change would make an older reader misinterpret a newer
    // file. Additive fields don't need it: missing keys read as their defaults.
    static constexpr int kFormatVersion = 1;

    // New projects are saved as .genie. The previous .veproj extension is still
    // opened and still recognised on save, because files written before the
    // rename are just as valid — an app that can't open its own older projects
    // is a worse outcome than a slightly untidy filter.
    static QString fileExtension() { return QStringLiteral("genie"); }
    static QString legacyFileExtension() { return QStringLiteral("veproj"); }

    static QString openFilter() {
        return QStringLiteral("GenieEditor Project (*.genie *.veproj)");
    }
    static QString saveFilter() {
        return QStringLiteral("GenieEditor Project (*.genie)");
    }

    // Whether `path` already ends in an extension this app owns, so Save As
    // doesn't turn "old.veproj" into "old.veproj.genie".
    static bool hasProjectExtension(const QString& path);

    // The identifier written into the file's "format" field. Files carrying the
    // pre-rename identifier are still accepted on load.
    static QString formatId() { return QStringLiteral("genieeditor-project"); }
    static QString legacyFormatId() { return QStringLiteral("videoeditor-project"); }
};
