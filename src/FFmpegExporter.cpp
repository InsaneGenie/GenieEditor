#include "FFmpegExporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

extern "C" {
#include <libavformat/avformat.h>
}
#include <QVector>
#include <QPair>
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

// How this ffmpeg wants to be handed a filtergraph stored in a file.
//
// There is no single spelling that works everywhere, and the two are mutually
// exclusive rather than merely preferred:
//
//   * up to FFmpeg 6:  -filter_complex_script FILE
//   * from FFmpeg 7:   -/filter_complex FILE   (the generic "read this option's
//                      value from a file" syntax; the old name was deprecated
//                      in 7, and REMOVED in 9 — it errors out with
//                      "Unrecognized option 'filter_complex_script'")
//
// Hardcoding either one breaks the other, and the version string can't be
// trusted to decide: distribution and nightly builds report things like
// "N-119403-g1234" or "6.1.1-3ubuntu5", so parsing it is guesswork.
enum class GraphFileOption { Modern, Legacy };

// Probes by RUNNING a trivial graph rather than by inspecting help text or
// version numbers — the only thing that actually settles the question is
// whether ffmpeg accepts it. Costs one process launch of a few milliseconds,
// cached for the life of the process, and it proves the whole mechanism works
// rather than merely that the option name parses.
GraphFileOption probeGraphFileOption(const QString& ffmpegPath) {
    static QString cachedFor;
    static GraphFileOption cached = GraphFileOption::Modern;
    if (cachedFor == ffmpegPath) return cached;

    QTemporaryFile probeGraph(QDir::temp().absoluteFilePath("veprobe_XXXXXX.txt"));
    probeGraph.setAutoRemove(true);
    bool wrote = false;
    if (probeGraph.open()) {
        probeGraph.write("nullsrc=s=16x16:d=0.04[v]");
        probeGraph.flush();
        wrote = true;
    }

    auto accepts = [&](const QString& optionName) {
        if (!wrote) return false;
        QProcess probe;
        probe.start(ffmpegPath, {"-hide_banner", "-nostdin",
                                 optionName, probeGraph.fileName(),
                                 "-map", "[v]", "-frames:v", "1", "-f", "null", "-"});
        if (!probe.waitForFinished(8000)) {
            probe.kill();
            probe.waitForFinished(1000);
            return false;
        }
        return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    };

    // Modern first: it's what any currently-supported ffmpeg wants, so the
    // common case costs one probe rather than two.
    if (accepts("-/filter_complex")) {
        cached = GraphFileOption::Modern;
    } else if (accepts("-filter_complex_script")) {
        cached = GraphFileOption::Legacy;
    } else {
        // Neither answered — most likely the probe itself couldn't run. Modern
        // is the better guess for an ffmpeg new enough to be worth supporting,
        // and the error surfaced from the real render will be specific anyway.
        cached = GraphFileOption::Modern;
    }

    cachedFor = ffmpegPath;
    return cached;
}

// Whether an overlay source is a multi-frame animation, which decides how the
// input is looped (see the overlay input args below).
//
// Decided by EXTENSION, not by opening the file. This runs while building the
// argument list, including for the dry-run that produces the previewable
// command, and the export must not depend on a decode succeeding at that
// point. GIF is the only animated format routed to overlay tracks on import,
// so the two agree by construction.
// Audio rate change, pitch-preserved.
//
// atempo is CHAINED rather than used once because a single instance is only
// well-conditioned over a limited range (historically 0.5–2.0, and quality
// degrades toward the extremes even where a wider range is accepted). Halving
// or doubling repeatedly until the remainder is inside that range gives the
// same overall factor built from steps the filter handles cleanly — the
// standard way to reach 4x or 8x without artefacts.
//
// Returns an empty string at 1x so the caller can skip the filter entirely.
QString atempoChain(double speed) {
    if (std::fabs(speed - 1.0) < 1e-6) return QString();

    QStringList stages;
    double remaining = speed;
    while (remaining > 2.0) { stages << "atempo=2.0"; remaining /= 2.0; }
    while (remaining < 0.5) { stages << "atempo=0.5"; remaining *= 2.0; }
    if (std::fabs(remaining - 1.0) > 1e-6) {
        stages << QString("atempo=%1").arg(QString::number(remaining, 'f', 6));
    }
    return stages.join(",");
}

// Channel count of a file's best audio stream (0 if unknown). Cached: a
// chunked render asks about the same few files hundreds of times.
int audioChannelsOf(const QString& path) {
    static QMutex mutex;
    static QHash<QString, int> cache;
    {
        QMutexLocker lock(&mutex);
        auto it = cache.constFind(path);
        if (it != cache.constEnd()) return it.value();
    }
    int channels = 0;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0) {
            const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (idx >= 0) channels = fmt->streams[idx]->codecpar->ch_layout.nb_channels;
        }
        avformat_close_input(&fmt);
    }
    QMutexLocker lock(&mutex);
    cache.insert(path, channels);
    return channels;
}

// Input arguments for one clip: seek to its in-point and read only its span.
//
// Every clip used to be opened as a bare `-i file` and cut with trim inside
// the graph — which means ffmpeg decoded each input FROM THE START OF THE FILE
// up to the in-point, only for trim to throw those frames away. Harmless for a
// clip near the start of a short file; for the hundredth cut of an 80-hour
// recording it's tens of hours of decoding per clip. Input seeking (-ss before
// -i) jumps to the nearest keyframe and decodes only from there, and ffmpeg's
// accurate_seek (on by default when transcoding) discards the lead-in so the
// first frame out is exactly the in-point, stamped 0.
//
// -t stops the input half a second past the out-point for the same reason in
// reverse: without it every input kept decoding to the end of its file.
QStringList seekedInputArgs(const Clip& clip) {
    QStringList args;
    if (clip.sourceInSec > 0.0005) args << "-ss" << num(clip.sourceInSec, 6);
    args << "-t" << num(clip.sourceDurationSec() + 0.5, 6) << "-i" << clip.sourcePath;
    return args;
}

bool isAnimatedOverlaySource(const QString& path) {
    return QFileInfo(path).suffix().compare("gif", Qt::CaseInsensitive) == 0;
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
                                         QStringList* inputArgs, bool* hasAudio,
                                         double totalOverrideSec, bool forceStereoAudio) {
    const double total = totalOverrideSec > 0.0 ? totalOverrideSec : std::max(0.1, project.durationSec());
    QStringList chains;
    int inputIndex = 0;

    // The canvas every clip composites onto. Establishing it explicitly (rather
    // than letting the first clip define the output) is what makes gaps render
    // as black instead of freezing or collapsing the timeline.
    chains << QString("color=c=black:s=%1x%2:r=%3:d=%4,format=yuv420p[bg0]")
                  .arg(options.width).arg(options.height)
                  // For a chunk (fixed length) the canvas runs a second long:
                  // the caller bounds the output with an exact frame count,
                  // and a canvas cut at a rounded duration could end one
                  // frame short of it.
                  .arg(num(options.fps, 3)).arg(num(totalOverrideSec > 0.0 ? total + 1.0 : total, 6));

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
            if (inputArgs) *inputArgs << seekedInputArgs(clip);

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
            // Speed is applied by dividing presentation timestamps: at 4x each
            // frame is stamped a quarter as far apart, so the same source
            // frames occupy a quarter of the time. It goes immediately after
            // the STARTPTS reset and before fps, so the fps filter resamples
            // the already-compressed stream to the output rate — the other
            // order would resample first and then compress, leaving the output
            // at four times the requested frame rate.
            const QString speedFilter = std::fabs(clip.effectiveSpeed() - 1.0) < 1e-6
                ? QString()
                : QString("setpts=PTS/%1,").arg(num(clip.effectiveSpeed(), 6));

            // trim is relative to the SEEKED input: with -ss before -i the
            // first frame is already the clip's in-point, stamped 0.
            chains << QString("[%1:v]trim=start=%2:end=%3,setpts=PTS-STARTPTS,%9"
                              "scale=%4:%5:force_original_aspect_ratio=decrease,"
                              "pad=%4:%5:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=%6,"
                              "format=yuv420p,tpad=start_duration=%7[%8]")
                          .arg(inputIndex)
                          .arg(num(0.0, 3)).arg(num(clip.sourceDurationSec(), 3))
                          .arg(options.width).arg(options.height)
                          .arg(num(options.fps, 3))
                          .arg(num(start, 3))
                          .arg(src)
                          .arg(speedFilter);

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

            // Declared up here because the input arguments below need to know
            // when this overlay stops being visible, in order to stop feeding
            // it frames past that point.
            const double start = clip.trackPosSec;
            const double end = start + clip.durationSec();

            // Fed for the clip's span rather than the whole project, so the
            // source has frames at every timestamp and the `enable` window is the
            // single thing deciding when it's visible. It removes an entire class
            // of PTS-alignment bug, and costs almost nothing.
            //
            // HOW it's looped depends on the file, and the two options are not
            // interchangeable. `-loop 1` belongs to the image2 demuxer, which
            // handles stills; the gif demuxer has no such option and ffmpeg
            // fails outright rather than ignoring it. Animations instead need
            // `-ignore_loop 0`, which tells the gif demuxer to honour the file's
            // own loop flag — nearly every GIF asks to repeat forever, so it
            // keeps supplying frames for as long as the timeline needs them.
            if (inputArgs) {
                if (isAnimatedOverlaySource(clip.sourcePath)) {
                    *inputArgs << "-ignore_loop" << "0";
                } else {
                    *inputArgs << "-loop" << "1";
                }
                // Bounded to the moment this overlay stops being visible rather
                // than to the whole project. The `enable` window already stops
                // it being COMPOSITED after that point, but the source chain --
                // format conversion, scale, rotate -- was still running on
                // every frame to the end of the timeline for an overlay that
                // might last two seconds. eof_action=pass on the overlay filter
                // handles the input ending early.
                //
                // A small margin past the end, because the filter graph works
                // in floating point and an input that stops at exactly the same
                // instant the window closes can drop the final frame.
                const double feedUntil = std::min(total, end + 0.5);
                *inputArgs << "-t" << num(feedUntil, 3) << "-i" << clip.sourcePath;
            }

            const QString src = QString("o%1").arg(stage);
            const QString out = QString("bg%1").arg(stage + 1);

            // Width is a fraction of the canvas; height follows the image's own
            // aspect, so an overlay never distorts at any output resolution.
            const QString scaleExpr = compileExpression(clip.anim.scale, start, "t");
            const QString wExpr = QString("max(2,trunc(%1*(%2)/2)*2)").arg(options.width).arg(scaleExpr);
            const QString hExpr = QString("max(2,trunc(%1*(%2)*ih/iw/2)*2)").arg(options.width).arg(scaleExpr);

            QString chain = QString("[%1:v]format=rgba").arg(inputIndex);

            // An ANIMATION has to be shifted so its first frame lands at the
            // clip's start on the timeline. Without this the source runs on
            // timeline time — a GIF placed at 0:05 would begin five seconds into
            // its own loop, showing a different frame than the preview, which
            // runs it on clip-relative time. Everything else about a clip
            // (keyframes, the enable window) is already clip-relative; the
            // animation has to be too or the two can't agree.
            //
            // tpad rather than a plain setpts shift for the same reason the
            // video chain above uses it: overlay stalls waiting on a second
            // input that has no frames yet. The padded frames are never seen —
            // the enable window starts exactly where they end.
            //
            // Placed BEFORE geq deliberately; the note below about nothing
            // coming between geq and scale still holds.
            if (isAnimatedOverlaySource(clip.sourcePath) && start > 0.0) {
                chain += QString(",setpts=PTS-STARTPTS,tpad=start_duration=%1:color=black@0")
                             .arg(num(start, 3));
            }

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

            // Scale next, and the eval mode is chosen rather than fixed.
            //
            // eval=frame re-evaluates the size expression on every frame, which
            // an ANIMATED scale genuinely needs. It also forces the scaler to
            // reconfigure each time, and that is expensive: measured on a 20s
            // 1080p export with one static overlay, using eval=frame where
            // eval=init would do cost 38% of the total export time -- 22.5s
            // against 14.0s -- for a size that never changed.
            //
            // Most overlays sit at one size for their whole life, so the common
            // case should not pay for the rare one.
            const bool scaleAnimated = clip.anim.scale.isAnimated();
            chain += QString(",scale=w='%1':h='%2':eval=%3")
                         .arg(wExpr, hExpr, scaleAnimated ? "frame" : "init");

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
            if (inputArgs) *inputArgs << seekedInputArgs(clip);

            const QString label = QString("a%1").arg(audioLabels.size());
            const int delayMs = static_cast<int>(clip.trackPosSec * 1000.0 + 0.5);

            // Per-clip and per-track gain are both in dB and sum, matching the
            // model's documented mixing math. aresample keeps every branch at a
            // single rate so amix isn't handed mismatched inputs.
            // atempo rather than asetpts, because this is the pitch-preserving
            // one — asetpts would resample and chipmunk the audio, which is
            // exactly the artefact that makes a sped-up section sound amateur.
            // Placed before aresample so the rate conversion happens once, on
            // the already-stretched stream.
            const QString tempo = atempoChain(clip.effectiveSpeed());

            // volumePercent is a linear preview multiplier. Convert it to dB
            // here so exported loudness matches the editor's per-track preview.
            const double linearTrackVolume = std::max(0.0, track.volumePercent / 100.0);
            const double percentDb = linearTrackVolume > 0.0
                ? 20.0 * std::log10(linearTrackVolume)
                : -120.0; // effectively silent without relying on -inf parsing
            const double exportGainDb = clip.gainDb + track.gainDb + percentDb;

            // Every clip is made stereo here, per clip, so the mix below (and
            // the pieces of a chunked render) all share one layout. Left to an
            // implicit conversion, a mono recording is treated as a CENTRE
            // channel and spread to left and right at -3dB — audibly quieter.
            // pan duplicates mono at its own level; anything else takes the
            // standard conversion.
            const QString toStereo = audioChannelsOf(clip.sourcePath) == 1
                ? QString(",pan=stereo|c0=c0|c1=c0")
                : QString(",aformat=channel_layouts=stereo");

            chains << QString("[%1:a]atrim=start=%2:end=%3,asetpts=PTS-STARTPTS,%7"
                              "aresample=48000,volume=%4dB%8,adelay=%5:all=1[%6]")
                          .arg(inputIndex)
                          .arg(num(0.0, 3)).arg(num(clip.sourceDurationSec(), 3))
                          .arg(num(exportGainDb, 2))
                          .arg(delayMs)
                          .arg(label)
                          .arg(tempo.isEmpty() ? QString() : tempo + ",")
                          .arg(toStereo);

            audioLabels << label;
            ++inputIndex;
        }
    }

    if (hasAudio) *hasAudio = !audioLabels.isEmpty() || forceStereoAudio;

    // Every clip is mixed onto a bed of silence exactly `total` long, and the
    // bed alone decides the output length (duration=first).
    //
    // This replaced `apad,atrim` on the mix, which HANGS when none of its
    // inputs ever produces a frame: apad waits forever for a first frame
    // before it can start padding. That's not hypothetical — a clip whose
    // in-point lies past the end of its file's audio (containers often report
    // a duration a little longer than their audio stream actually runs) yields
    // nothing, and a render made only of such clips stalled at frame 0 with
    // no error. The bed always flows, so the mix always has something to end
    // on. It also covers pieces of a chunked render that have no audio at all,
    // which still need a silent track to be joinable.
    //
    // normalize=0 keeps each clip at the level the user actually set; amix's
    // default normalisation would divide everything by the input count.
    if (!audioLabels.isEmpty() || forceStereoAudio) {
        QString inputs = "[bed]";
        for (const QString& l : audioLabels) inputs += QString("[%1]").arg(l);
        chains << QString("anullsrc=r=48000:cl=stereo,atrim=0:%1[bed]").arg(num(total, 6));
        chains << QString("%1amix=inputs=%2:normalize=0:duration=first:dropout_transition=0,"
                          "aformat=sample_fmts=s16:sample_rates=48000:channel_layouts=stereo[aout]")
                      .arg(inputs).arg(audioLabels.size() + 1);
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
    // Adapts the plain callback onto the detailed one, so there is a single
    // implementation rather than two that can drift apart.
    return exportProject(project, options, [&](const Progress& p) {
        return onProgress ? onProgress(p.fraction) : true;
    });
}

bool FFmpegExporter::exportProject(const Project& project, const Options& options,
                                   DetailedProgressFn onProgress) {
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
        // Naming the folder it actually looked in turns this from a dead end
        // into something checkable — the usual cause is simply that the
        // executable never got copied next to the app, and seeing the path
        // makes that obvious rather than a guess.
        m_error = QString(
            "Couldn't find the ffmpeg program.\n\n"
            "Note this is the ffmpeg EXECUTABLE, which is separate from the FFmpeg "
            "libraries this app is built against — so exporting can fail even though "
            "everything else works.\n\n"
            "Looked for it here:\n"
            "  %1\n"
            "  (and its ffmpeg\\ and bin\\ subfolders)\n"
            "  ...then on your PATH.\n\n"
            "Put ffmpeg.exe in that folder, or install it and make sure it's on your "
            "PATH, then try again.")
            .arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()));
        return false;
    }

    // --- How to render -----------------------------------------------------
    // Every clip is its own ffmpeg input, and that doesn't scale past a point:
    // each input is an argument pair on the command line, which Windows caps
    // at ~32k characters (a timeline cut into a few hundred pieces blows past
    // it, and CreateProcess then refuses to start ffmpeg at all), and each is a
    // decoder held open for the whole render.
    //
    // So a busy timeline is rendered in CHUNKS: consecutive time ranges, each
    // touching at most kMaxInputsPerChunk clips, rendered one after another
    // with the same encoder settings, then joined losslessly (video stream-
    // copied, not re-encoded). A simple timeline still renders in one pass.
    constexpr int kMaxInputsPerChunk = 24;

    struct Span { double start; double end; };
    QVector<Span> clipSpans;
    bool projectHasAudio = false;
    for (const Track& track : project.tracks) {
        if (!track.enabled) continue;
        const bool audible = track.type == TrackType::Audio && !track.muted;
        if (track.type == TrackType::Audio && !audible) continue;
        for (const Clip& clip : track.clips) {
            if (clip.durationSec() <= 0.0) continue;
            clipSpans.push_back({clip.trackPosSec, clip.trackPosSec + clip.durationSec()});
            if (audible) projectHasAudio = true;
        }
    }
    if (clipSpans.isEmpty()) {
        m_error = "There's nothing to export yet — no clips on any enabled track.";
        return false;
    }

    QElapsedTimer wallClock;
    wallClock.start();
    Progress lastProgress;
    const GraphFileOption graphOption = probeGraphFileOption(ffmpeg);

    // Runs one ffmpeg process to completion, reporting progress for a slice of
    // the overall job: rendered seconds are offset by `timelineOffset`, and the
    // fraction is mapped into [fracFrom, fracFrom + fracSpan].
    auto runFfmpeg = [&](const QStringList& args, double segmentSec, double timelineOffset,
                         double fracFrom, double fracSpan, const QString& partialOutput) -> bool {
        QProcess process;
        process.setProgram(ffmpeg);
        process.setArguments(args);
        // Merged so a failure's diagnostics are captured in one stream; ffmpeg
        // writes progress to stdout and errors to stderr.
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start();

        if (!process.waitForStarted(10000)) {
            // errorString says WHY — "The filename or extension is too long"
            // is the command-line limit, for instance — where the old message
            // only said that it didn't start.
            m_error = QString("Couldn't start ffmpeg (%1).\n\n%2").arg(ffmpeg, process.errorString());
            return false;
        }

        QString tail;
        bool cancelled = false;
        while (process.state() != QProcess::NotRunning) {
            process.waitForReadyRead(200);
            const QString chunk = QString::fromUtf8(process.readAll());
            if (chunk.isEmpty()) continue;
            tail += chunk;
            // Only the last stretch is kept: a long render produces megabytes of
            // progress lines, and all that's wanted on failure is the end of it.
            if (tail.size() > 8000) tail = tail.right(6000);

            // -progress emits a block of key=value lines per update; the last
            // value of each key in this chunk is the most recent one.
            auto lastValue = [&chunk](const QString& key) -> QString {
                const int idx = chunk.lastIndexOf(key);
                if (idx < 0) return QString();
                const int from = idx + key.size();
                const int lineEnd = chunk.indexOf('\n', from);
                return chunk.mid(from, lineEnd < 0 ? -1 : lineEnd - from).trimmed();
            };

            if (onProgress && chunk.contains("out_time_us=")) {
                bool ok = false;
                const qlonglong us = lastValue("out_time_us=").toLongLong(&ok);
                if (ok && us >= 0) {
                    Progress p;
                    const double segDone = segmentSec > 0.0 ? std::clamp(us / 1e6 / segmentSec, 0.0, 1.0) : 0.0;
                    p.totalSec = total;
                    p.renderedSec = std::min(total, timelineOffset + us / 1e6);
                    p.fraction = std::clamp(fracFrom + fracSpan * segDone, 0.0, 1.0);
                    p.elapsedSec = wallClock.elapsed() / 1000.0;
                    p.frames = lastValue("frame=").toLongLong();
                    p.fps = lastValue("fps=").toDouble();
                    p.outputBytes = lastValue("total_size=").toLongLong();
                    // "speed=" arrives with a trailing x, and reads N/A until
                    // the first frames are through.
                    QString speed = lastValue("speed=");
                    if (speed.endsWith('x')) speed.chop(1);
                    p.speed = speed.toDouble(); // 0 on N/A, which callers treat as unknown

                    lastProgress = p;
                    if (!onProgress(p)) {
                        cancelled = true;
                        break;
                    }
                }
            }
        }

        if (cancelled) {
            process.kill();
            process.waitForFinished(3000);
            QFile::remove(partialOutput); // a half-written file is worse than none
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
        return true;
    };

    // Writes a graph to a temp file and returns the args that point at it.
    // The graph goes in a file rather than on the command line: Windows caps a
    // command line at ~32k characters. Which OPTION does that differs by ffmpeg
    // version — see probeGraphFileOption.
    QVector<QTemporaryFile*> graphFiles; // kept alive until the renders finish
    auto graphFileArgs = [&](const QString& graph) -> QStringList {
        auto* file = new QTemporaryFile(QDir::temp().absoluteFilePath("veexport_XXXXXX.txt"));
        file->setAutoRemove(true);
        graphFiles.push_back(file);
        if (!file->open()) return {};
        file->write(graph.toUtf8());
        file->flush();
        return {graphOption == GraphFileOption::Modern ? "-/filter_complex" : "-filter_complex_script",
                file->fileName()};
    };
    struct GraphFileCleanup {
        QVector<QTemporaryFile*>& files;
        ~GraphFileCleanup() { qDeleteAll(files); }
    } graphCleanup{graphFiles};

    auto videoEncodeArgs = [&]() {
        return QStringList{"-c:v", options.videoCodec, "-preset", options.preset,
                           "-crf", QString::number(options.crf), "-pix_fmt", "yuv420p",
                           "-r", num(options.fps, 3)};
    };

    // --- Single pass --------------------------------------------------------
    if (clipSpans.size() <= kMaxInputsPerChunk) {
        QStringList inputArgs;
        bool hasAudio = false;
        const QString graph = buildFilterGraph(project, options, &inputArgs, &hasAudio);
        m_lastGraph = graph;

        const QStringList graphArgs = graphFileArgs(graph);
        if (graphArgs.isEmpty()) {
            m_error = "Couldn't write a temporary file for the render settings.";
            return false;
        }

        QStringList args{"-hide_banner", "-nostdin", "-y"};
        args << inputArgs << graphArgs << "-map" << "[vout]";
        if (hasAudio) args << "-map" << "[aout]";
        args << videoEncodeArgs();
        if (hasAudio) {
            args << "-c:a" << options.audioCodec
                 << "-b:a" << QString("%1k").arg(options.audioBitrateKbps);
        }
        // -t bounds the render to the project length. Without it a looped
        // overlay input can keep the encode running past the end of the timeline.
        args << "-t" << num(total, 3) << "-progress" << "pipe:1" << "-nostats" << options.outputPath;

        if (!runFfmpeg(args, total, 0.0, 0.0, 1.0, options.outputPath)) return false;
    } else {
        // --- Chunked --------------------------------------------------------
        // Chunk edges sit on clip boundaries where possible (so fewer clips
        // are split) and always on whole frames, so every chunk is an exact
        // number of frames and the joined video has no drift or dropped frame
        // at the seams.
        const double fps = options.fps > 0.0 ? options.fps : 30.0;
        const qint64 totalFrames = std::max<qint64>(1, std::llround(total * fps));
        auto toFrame = [&](double sec) { return std::clamp<qint64>(std::llround(sec * fps), 0, totalFrames); };

        QVector<qint64> candidates{totalFrames};
        for (const Span& sp : clipSpans) {
            candidates << toFrame(sp.start) << toFrame(sp.end);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        auto clipsTouching = [&](qint64 a, qint64 b) {
            const double as = a / fps, bs = b / fps;
            int n = 0;
            for (const Span& sp : clipSpans) {
                if (sp.end > as + 1e-6 && sp.start < bs - 1e-6) ++n;
            }
            return n;
        };

        QVector<QPair<qint64, qint64>> chunks;
        qint64 a = 0;
        while (a < totalFrames) {
            qint64 best = -1;
            for (qint64 b : candidates) {
                if (b <= a) continue;
                if (best < 0 || clipsTouching(a, b) <= kMaxInputsPerChunk) best = b;
                else break;
            }
            if (best < 0) best = totalFrames;
            chunks.push_back({a, best});
            a = best;
        }

        // Intermediates live beside the output, not in %TEMP%: they add up to
        // roughly the size of the finished video, and the output's drive is the
        // one the user chose to have room on. Removed automatically afterwards.
        QTemporaryDir workDir(QFileInfo(options.outputPath).absolutePath() + "/.genie-export-XXXXXX");
        // Diagnostic: set GENIE_KEEP_EXPORT_PARTS=1 to leave the parts on disk
        // for inspection when a join goes wrong.
        if (qEnvironmentVariableIsSet("GENIE_KEEP_EXPORT_PARTS")) workDir.setAutoRemove(false);
        if (!workDir.isValid()) {
            m_error = QString("Couldn't create a working folder next to the output (%1).")
                          .arg(QFileInfo(options.outputPath).absolutePath());
            return false;
        }

        // Rendering is the bulk of the work; joining is a fast copy.
        constexpr double kRenderShare = 0.97;
        QStringList concatList;

        for (int ci = 0; ci < chunks.size(); ++ci) {
            const qint64 fa = chunks[ci].first, fb = chunks[ci].second;
            const double startSec = fa / fps;
            const double lengthSec = (fb - fa) / fps;

            // The chunk as a project of its own: everything outside its range
            // ripple-deleted, which trims straddling clips through their own
            // time mapping (speed included) and shifts overlay keyframes.
            Project piece = project;
            piece.rippleDeleteRange(fb / fps, total + 3600.0);
            piece.rippleDeleteRange(0.0, startSec);

            QStringList inputArgs;
            bool hasAudio = false;
            const QString graph = buildFilterGraph(piece, options, &inputArgs, &hasAudio,
                                                   lengthSec, /*forceStereoAudio=*/projectHasAudio);
            m_lastGraph = graph;
            const QStringList graphArgs = graphFileArgs(graph);
            if (graphArgs.isEmpty()) {
                m_error = "Couldn't write a temporary file for the render settings.";
                return false;
            }

            const QString chunkPath = workDir.filePath(QString("chunk_%1.mkv").arg(ci, 5, 10, QChar('0')));
            QStringList args{"-hide_banner", "-nostdin", "-y"};
            args << inputArgs << graphArgs << "-map" << "[vout]";
            if (hasAudio) args << "-map" << "[aout]";
            args << videoEncodeArgs();
            // FLAC for the intermediate audio: lossless and sample-exact, so
            // the pieces join without the gaps or clicks a lossy codec's
            // encoder delay leaves at every seam. Encoded to the real audio
            // codec once, during the join.
            if (hasAudio) args << "-c:a" << "flac";
            // A frame count, not a duration: a float -t can land a hair short
            // and drop the last frame of every chunk.
            args << "-frames:v" << QString::number(fb - fa)
                 << "-progress" << "pipe:1" << "-nostats" << chunkPath;

            const double fracFrom = kRenderShare * startSec / total;
            const double fracSpan = kRenderShare * lengthSec / total;
            if (!runFfmpeg(args, lengthSec, startSec, fracFrom, fracSpan, chunkPath)) {
                if (!m_error.startsWith("Export cancelled"))
                    m_error = QString("Rendering part %1 of %2 failed.\n\n%3").arg(ci + 1).arg(chunks.size()).arg(m_error);
                return false;
            }
            concatList << QString("file '%1'").arg(QFileInfo(chunkPath).fileName());
        }

        const QString listPath = workDir.filePath("parts.txt");
        {
            QFile list(listPath);
            if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                m_error = "Couldn't write the list of rendered parts.";
                return false;
            }
            list.write((concatList.join('\n') + '\n').toUtf8());
        }

        QStringList args{"-hide_banner", "-nostdin", "-y", "-f", "concat", "-safe", "0", "-i", listPath,
                         "-map", "0:v"};
        if (projectHasAudio) args << "-map" << "0:a";
        args << "-c:v" << "copy";
        if (projectHasAudio) {
            args << "-c:a" << options.audioCodec
                 << "-b:a" << QString("%1k").arg(options.audioBitrateKbps);
        }
        args << "-progress" << "pipe:1" << "-nostats" << options.outputPath;
        if (!runFfmpeg(args, total, 0.0, kRenderShare, 1.0 - kRenderShare, options.outputPath)) {
            if (!m_error.startsWith("Export cancelled"))
                m_error = QString("Joining the rendered parts failed.\n\n%1").arg(m_error);
            return false;
        }
    }

    if (!QFileInfo::exists(options.outputPath)) {
        m_error = "ffmpeg reported success but produced no output file.";
        return false;
    }

    if (onProgress) {
        Progress done = lastProgress;
        done.fraction = 1.0;
        done.totalSec = total;
        done.renderedSec = total;
        done.elapsedSec = wallClock.elapsed() / 1000.0;
        // Recomputed over the whole run rather than kept from the last update:
        // this is the figure worth remembering.
        if (done.elapsedSec > 0.01) done.speed = total / done.elapsedSec;
        onProgress(done);
    }
    return true;
}
