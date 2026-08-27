#include "FFmpegExporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// FFmpeg expression syntax has no string escaping worth relying on, so every
// number goes in with a fixed, locale-independent representation. QString::arg
// with an explicit format avoids a comma decimal separator on e.g. a German
// system quietly corrupting every expression in the graph.
QString num(double v, int decimals = 4) {
    return QString::number(v, 'f', decimals);
}

} // namespace

QString FFmpegExporter::compileExpression(const AnimatedProperty& prop, double clipStartSec,
                                          const QString& timeVar, double multiplier) {
    if (prop.keys.isEmpty()) return num(prop.staticValue * multiplier);
    if (prop.keys.size() == 1) return num(prop.keys.first().value * multiplier);

    // Clip-relative time. Keyframes are stored relative to the clip's own start
    // (so moving a clip carries its animation), but filters see absolute
    // timeline time — this is where the two are reconciled.
    const QString localTime = QString("(%1-%2)").arg(timeVar, num(clipStartSec));

    // Built inside-out into a chain of nested if()s: one branch per keyframe
    // interval, each linearly interpolating across it, with the first and last
    // values held flat beyond the ends.
    //
    // Assembled from the LAST interval backwards, because each if()'s else
    // branch is the whole remaining chain — building forwards would mean
    // repeatedly rewriting the innermost term.
    QString expr = num(prop.keys.last().value * multiplier);

    for (int i = prop.keys.size() - 1; i >= 1; --i) {
        const Keyframe& a = prop.keys[i - 1];
        const Keyframe& b = prop.keys[i];
        const double span = b.timeSec - a.timeSec;

        QString segment;
        if (span <= 1e-9) {
            segment = num(b.value * multiplier); // coincident keys — a step, not a ramp
        } else {
            segment = QString("(%1+(%2)*((%3-%4)/%5))")
                          .arg(num(a.value * multiplier))
                          .arg(num((b.value - a.value) * multiplier))
                          .arg(localTime)
                          .arg(num(a.timeSec))
                          .arg(num(span));
        }

        expr = QString("if(lt(%1,%2),%3,%4)")
                   .arg(localTime, num(b.timeSec), segment, expr);
    }

    // Before the first key, hold the first value.
    expr = QString("if(lt(%1,%2),%3,%4)")
               .arg(localTime, num(prop.keys.first().timeSec),
                    num(prop.keys.first().value * multiplier), expr);
    return expr;
}

QString FFmpegExporter::buildFilterGraph(const Project& project, const Options& options,
                                         QStringList* inputArgs, bool* hasAudio) {
    const double total = std::max(0.1, project.durationSec());
    QStringList chains;
    int inputIndex = 0;

    // The canvas every clip composites onto. Establishing it explicitly (rather
    // than letting the first clip define the output) is what makes gaps render
    // as black instead of freezing or collapsing the timeline.
    chains << QString("color=c=black:s=%1x%2:r=%3:d=%4,format=yuv420p[bg0]")
                  .arg(options.width).arg(options.height)
                  .arg(num(options.fps, 3)).arg(num(total, 3));

    QString videoLabel = "bg0";
    int stage = 0;
    QStringList audioLabels;

    // --- Video tracks -------------------------------------------------------
    // Ascending index order: later tracks composite last and therefore sit on
    // top, matching MainWindow's preview priority rule.
    for (int t = 0; t < project.tracks.size(); ++t) {
        const Track& track = project.tracks[t];
        if (track.type != TrackType::Video || !track.enabled) continue;

        for (const Clip& clip : track.clips) {
            if (clip.durationSec() <= 0.0) continue;
            if (inputArgs) *inputArgs << "-i" << clip.sourcePath;

            const double start = clip.trackPosSec;
            const double end = start + clip.durationSec();
            const QString src = QString("v%1").arg(stage);
            const QString out = QString("bg%1").arg(stage + 1);

            // force_original_aspect_ratio + pad letterboxes rather than
            // stretching footage whose shape differs from the canvas. tpad
            // shifts the clip to its timeline position by padding black in
            // front of it, which also guarantees the overlay filter has frames
            // available from t=0 — without it, overlay stalls waiting for a
            // second input that doesn't begin until later.
            chains << QString("[%1:v]trim=start=%2:end=%3,setpts=PTS-STARTPTS,"
                              "scale=%4:%5:force_original_aspect_ratio=decrease,"
                              "pad=%4:%5:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=%6,"
                              "format=yuv420p,tpad=start_duration=%7[%8]")
                          .arg(inputIndex)
                          .arg(num(clip.sourceInSec, 3)).arg(num(clip.sourceOutSec, 3))
                          .arg(options.width).arg(options.height)
                          .arg(num(options.fps, 3))
                          .arg(num(start, 3))
                          .arg(src);

            chains << QString("[%1][%2]overlay=x=0:y=0:eof_action=pass:repeatlast=0:"
                              "enable='between(t,%3,%4)'[%5]")
                          .arg(videoLabel, src, num(start, 3), num(end, 3), out);

            videoLabel = out;
            ++stage;
            ++inputIndex;
        }
    }

    // --- Overlay tracks -----------------------------------------------------
    for (int t = 0; t < project.tracks.size(); ++t) {
        const Track& track = project.tracks[t];
        if (track.type != TrackType::Overlay || !track.enabled) continue;

        for (const Clip& clip : track.clips) {
            if (clip.durationSec() <= 0.0) continue;

            // Looped for the whole project rather than just the clip's span, so
            // the still has frames at every timestamp and the `enable` window is
            // the single thing deciding when it's visible. One static frame held
            // by a decoder costs almost nothing, and it removes an entire class
            // of PTS-alignment bug.
            if (inputArgs) *inputArgs << "-loop" << "1" << "-t" << num(total, 3) << "-i" << clip.sourcePath;

            const double start = clip.trackPosSec;
            const double end = start + clip.durationSec();
            const QString src = QString("o%1").arg(stage);
            const QString out = QString("bg%1").arg(stage + 1);

            // Width is a fraction of the canvas; height follows the image's own
            // aspect, so an overlay never distorts at any output resolution.
            const QString scaleExpr = compileExpression(clip.anim.scale, start, "t");
            const QString wExpr = QString("max(2,trunc(%1*(%2)/2)*2)").arg(options.width).arg(scaleExpr);
            const QString hExpr = QString("max(2,trunc(%1*(%2)*ih/iw/2)*2)").arg(options.width).arg(scaleExpr);

            QString chain = QString("[%1:v]format=rgba").arg(inputIndex);

            // Opacity is applied BEFORE the scale, and the ordering is load-
            // bearing rather than stylistic: geq cannot cope with input whose
            // dimensions change from frame to frame, and an animated scale does
            // exactly that. Running geq first — at the image's fixed native size
            // — sidesteps it, and is cheaper besides, since the alpha maths
            // happens at source resolution instead of on an upscaled copy.
            //
            // Nothing may be inserted between geq and scale here; an
            // intermediate format filter in that position crashed ffmpeg
            // outright rather than erroring cleanly.
            //
            // A constant opacity gets colorchannelmixer, which is nearly free.
            // Only a genuinely animated one pays for geq, which evaluates an
            // expression per pixel per frame and is by far the most expensive
            // filter in this graph.
            if (clip.anim.opacity.isAnimated()) {
                chain += QString(",geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='alpha(X,Y)*(%1)'")
                             .arg(compileExpression(clip.anim.opacity, start, "T"));
            } else {
                const double op = std::clamp(clip.anim.opacity.valueAt(0.0), 0.0, 1.0);
                if (op < 0.999) chain += QString(",colorchannelmixer=aa=%1").arg(num(op));
            }

            // Scale next. eval=frame is what makes the expression re-evaluate
            // per frame instead of being frozen at initialisation.
            chain += QString(",scale=w='%1':h='%2':eval=frame").arg(wExpr, hExpr);

            // Rotation last, and only when there's any. c=none keeps the corners
            // it exposes transparent instead of black, and the output canvas is
            // widened to the input's diagonal so a rotated image never has its
            // corners clipped off. Because rotate expands symmetrically about the
            // centre, and the overlay is positioned by its centre, the placement
            // maths below needs no adjustment.
            //
            // Verified that rotate tolerates the per-frame size changes coming
            // out of an animated scale — unlike geq, which is why THAT one has to
            // run before the scale rather than after.
            const bool rotates = clip.anim.rotation.isAnimated()
                              || std::abs(clip.anim.rotation.valueAt(0.0)) > 0.01;
            if (rotates) {
                // FFmpeg's rotate takes radians; the model stores degrees because
                // that's what anyone editing actually thinks in.
                const QString angleExpr =
                    compileExpression(clip.anim.rotation, start, "t", M_PI / 180.0);
                chain += QString(",rotate=a='%1':c=none:ow='hypot(iw,ih)':oh='hypot(iw,ih)'")
                             .arg(angleExpr);
            }

            chain += QString("[%1]").arg(src);
            chains << chain;

            // x/y address the overlay's CENTRE, so motion behaves the same
            // regardless of the image's size — hence the -w/2 and -h/2, where w
            // and h are overlay's own (post-scale) dimensions.
            const QString xExpr = QString("(%1)-w/2")
                .arg(compileExpression(clip.anim.x, start, "t", options.width));
            const QString yExpr = QString("(%1)-h/2")
                .arg(compileExpression(clip.anim.y, start, "t", options.height));

            chains << QString("[%1][%2]overlay=x='%3':y='%4':eof_action=pass:"
                              "enable='between(t,%5,%6)'[%7]")
                          .arg(videoLabel, src, xExpr, yExpr, num(start, 3), num(end, 3), out);

            videoLabel = out;
            ++stage;
            ++inputIndex;
        }
    }

    chains << QString("[%1]format=yuv420p[vout]").arg(videoLabel);

    // --- Audio --------------------------------------------------------------
    for (int t = 0; t < project.tracks.size(); ++t) {
        const Track& track = project.tracks[t];
        if (track.type != TrackType::Audio || !track.enabled || track.muted) continue;

        for (const Clip& clip : track.clips) {
            if (clip.durationSec() <= 0.0) continue;
            if (inputArgs) *inputArgs << "-i" << clip.sourcePath;

            const QString label = QString("a%1").arg(audioLabels.size());
            const int delayMs = static_cast<int>(clip.trackPosSec * 1000.0 + 0.5);

            // Per-clip and per-track gain are both in dB and sum, matching the
            // model's documented mixing math. aresample keeps every branch at a
            // single rate so amix isn't handed mismatched inputs.
            chains << QString("[%1:a]atrim=start=%2:end=%3,asetpts=PTS-STARTPTS,"
                              "aresample=48000,volume=%4dB,adelay=%5:all=1[%6]")
                          .arg(inputIndex)
                          .arg(num(clip.sourceInSec, 3)).arg(num(clip.sourceOutSec, 3))
                          .arg(num(clip.gainDb + track.gainDb, 2))
                          .arg(delayMs)
                          .arg(label);

            audioLabels << label;
            ++inputIndex;
        }
    }

    if (hasAudio) *hasAudio = !audioLabels.isEmpty();

    if (audioLabels.size() == 1) {
        chains << QString("[%1]apad,atrim=0:%2[aout]").arg(audioLabels.first(), num(total, 3));
    } else if (audioLabels.size() > 1) {
        QString inputs;
        for (const QString& l : audioLabels) inputs += QString("[%1]").arg(l);
        // normalize=0 keeps each track at the level the user actually set;
        // amix's default normalisation would quietly divide everything by the
        // number of tracks, so adding a track would turn the others down.
        chains << QString("%1amix=inputs=%2:normalize=0:dropout_transition=0,"
                          "apad,atrim=0:%3[aout]")
                      .arg(inputs).arg(audioLabels.size()).arg(num(total, 3));
    }

    return chains.join(";\n");
}

QString FFmpegExporter::resolveFfmpegPath() {
    const QString exeName =
#ifdef Q_OS_WIN
        "ffmpeg.exe";
#else
        "ffmpeg";
#endif

    // Alongside the application first: that's where vcpkg's applocal deploy and
    // any hand-assembled release folder put it, and a bundled copy should always
    // win over whatever unrelated version happens to be on the system PATH.
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const QString& candidate : {exeName, QString("ffmpeg/") + exeName, QString("bin/") + exeName}) {
        const QString path = appDir.absoluteFilePath(candidate);
        if (QFileInfo(path).isExecutable()) return path;
    }

    return QStandardPaths::findExecutable("ffmpeg");
}

bool FFmpegExporter::exportProject(const Project& project, const Options& options,
                                   ProgressFn onProgress) {
    m_error.clear();

    if (options.outputPath.isEmpty()) {
        m_error = "No output path was given.";
        return false;
    }

    const double total = project.durationSec();
    if (total <= 0.0) {
        m_error = "There's nothing to export yet — the timeline is empty.";
        return false;
    }

    const QString ffmpeg = resolveFfmpegPath();
    if (ffmpeg.isEmpty()) {
        m_error = "Couldn't find the ffmpeg program.\n\n"
                  "Put ffmpeg.exe next to this application, or install it and make sure "
                  "it's on your PATH, then try again.";
        return false;
    }

    QStringList inputArgs;
    bool hasAudio = false;
    const QString graph = buildFilterGraph(project, options, &inputArgs, &hasAudio);
    m_lastGraph = graph;

    if (inputArgs.isEmpty()) {
        m_error = "There's nothing to export yet — no clips on any enabled track.";
        return false;
    }

    // The graph goes in a file rather than on the command line. Windows caps a
    // command line at ~32k characters and a project of any size blows straight
    // past that; -filter_complex_script sidesteps the limit entirely.
    QTemporaryFile graphFile(QDir::temp().absoluteFilePath("veexport_XXXXXX.txt"));
    graphFile.setAutoRemove(true);
    if (!graphFile.open()) {
        m_error = "Couldn't write a temporary file for the render settings.";
        return false;
    }
    graphFile.write(graph.toUtf8());
    graphFile.flush();

    QStringList args;
    args << "-hide_banner" << "-nostdin" << "-y";
    args << inputArgs;
    args << "-filter_complex_script" << graphFile.fileName();
    args << "-map" << "[vout]";
    if (hasAudio) args << "-map" << "[aout]";

    args << "-c:v" << options.videoCodec
         << "-preset" << options.preset
         << "-crf" << QString::number(options.crf)
         << "-pix_fmt" << "yuv420p"
         << "-r" << num(options.fps, 3);

    if (hasAudio) {
        args << "-c:a" << options.audioCodec
             << "-b:a" << QString("%1k").arg(options.audioBitrateKbps);
    }

    // -t bounds the render to the project length. Without it a looped overlay
    // input can keep the encode running past the end of the timeline.
    args << "-t" << num(total, 3);
    args << "-progress" << "pipe:1" << "-nostats";
    args << options.outputPath;

    QProcess process;
    process.setProgram(ffmpeg);
    process.setArguments(args);
    // Merged so a failure's diagnostics are captured in one stream; ffmpeg
    // writes progress to stdout and errors to stderr.
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(10000)) {
        m_error = QString("Couldn't start ffmpeg (%1).").arg(ffmpeg);
        return false;
    }

    QString tail;
    bool cancelled = false;

    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        const QString chunk = QString::fromUtf8(process.readAll());
        if (!chunk.isEmpty()) {
            tail += chunk;
            // Only the last stretch is kept: a long render produces megabytes of
            // progress lines, and all that's wanted on failure is the end of it.
            if (tail.size() > 8000) tail = tail.right(6000);

            // -progress emits out_time_us=<microseconds> once per update.
            int idx = chunk.lastIndexOf("out_time_us=");
            if (idx >= 0 && onProgress) {
                const int lineEnd = chunk.indexOf('\n', idx);
                const QString value = chunk.mid(idx + 12, lineEnd < 0 ? -1 : lineEnd - idx - 12).trimmed();
                bool ok = false;
                const qlonglong us = value.toLongLong(&ok);
                if (ok && us >= 0) {
                    if (!onProgress(std::clamp(us / 1e6 / total, 0.0, 1.0))) {
                        cancelled = true;
                        break;
                    }
                }
            }
        }
    }

    if (cancelled) {
        process.kill();
        process.waitForFinished(3000);
        QFile::remove(options.outputPath); // a half-written file is worse than none
        m_error = "Export cancelled.";
        return false;
    }

    process.waitForFinished(-1);
    tail += QString::fromUtf8(process.readAll());

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        m_error = QString("ffmpeg failed (exit code %1).\n\n%2")
                      .arg(process.exitCode())
                      .arg(tail.right(1200).trimmed());
        return false;
    }

    if (!QFileInfo::exists(options.outputPath)) {
        m_error = "ffmpeg reported success but produced no output file.";
        return false;
    }

    if (onProgress) onProgress(1.0);
    return true;
}
