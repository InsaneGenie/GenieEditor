#pragma once

#include <QString>
#include <QStringList>
#include <functional>
#include "Project.h"

// Renders a Project's EDL to a single output file.
//
// --- Why this drives the ffmpeg binary rather than libav* in-process ---------
//
// The original sketch here planned an in-process libavfilter pipeline. The
// filter GRAPH is the hard part either way and is identical in both designs —
// what differs is only who executes it. Driving the binary buys:
//
//   * progress and cancellation almost for free (parse -progress, kill the
//     process), where in-process means hand-threading an encode loop;
//   * a graph that can be dumped and reproduced verbatim on the command line,
//     which makes an export bug debuggable rather than a mystery inside a
//     several-hundred-line decode/encode loop;
//   * far less code whose failure modes are subtle memory bugs.
//
// The cost is a runtime dependency on the ffmpeg executable — see
// resolveFfmpegPath() for where it's looked for. If the in-process route is ever
// wanted, buildFilterGraph() is exactly what would be handed to
// avfilter_graph_parse2, and none of it would need rewriting.
//
// --- How the graph is shaped ------------------------------------------------
//
// One `color` source establishes the output canvas, then every clip is
// composited onto it with `overlay`, gated by an `enable=` window so it appears
// only across its own span of the timeline. Video tracks are drawn in ascending
// index order so later tracks land on top, matching the priority layering the
// preview already uses. Audio clips are trimmed, gain-adjusted, delayed to their
// timeline position and summed with `amix`.
class FFmpegExporter {
public:
    struct Options {
        QString outputPath;
        int width = 1920;
        int height = 1080;
        double fps = 30.0;
        int crf = 18;              // x264 quality; lower is better, 18 is near-transparent
        QString preset = "medium";
        int audioBitrateKbps = 192;
        QString videoCodec = "libx264";
        QString audioCodec = "aac";
    };

    // Called with 0..1 as the render proceeds. Return false to cancel, which
    // terminates ffmpeg and removes the partial output file.
    // What ffmpeg reports while rendering, beyond bare percentage.
    //
    // All of this already arrives on the -progress pipe; the exporter used to
    // read out_time_us and throw the rest away. Surfacing it turns a bar that
    // could be doing anything into something you can judge: whether an export
    // is worth waiting for, and whether a settings change actually helped.
    struct Progress {
        double fraction = 0.0;      // 0..1, from out_time against project length
        double renderedSec = 0.0;   // position within the output
        double totalSec = 0.0;      // project length
        double speed = 0.0;         // multiple of realtime, ffmpeg's own "speed="
        double fps = 0.0;           // encoded frames per second
        qint64 frames = 0;
        qint64 outputBytes = 0;
        double elapsedSec = 0.0;

        // Remaining time, estimated from the rate achieved SO FAR rather than
        // instantaneously. Instantaneous speed swings wildly between scenes and
        // produces an estimate that jumps around; averaging over the whole run
        // gives a figure that settles. Negative when not yet estimable.
        double estimatedRemainingSec() const {
            if (fraction <= 0.001 || elapsedSec <= 0.5) return -1.0;
            return elapsedSec * (1.0 - fraction) / fraction;
        }
    };

    // Return false to cancel. The double overload is kept because it reads
    // better at call sites that only want the bar.
    using ProgressFn = std::function<bool(double)>;
    using DetailedProgressFn = std::function<bool(const Progress&)>;

    bool exportProject(const Project& project, const Options& options, ProgressFn onProgress = {});
    bool exportProject(const Project& project, const Options& options, DetailedProgressFn onProgress);

    const QString& errorMessage() const { return m_error; }

    // The graph that was last built. Exposed for diagnostics: it can be pasted
    // straight into an ffmpeg command line to reproduce a failed render.
    const QString& lastFilterGraph() const { return m_lastGraph; }

    // Absolute path to a usable ffmpeg executable, or empty if none was found.
    // Checks alongside the application first (the vcpkg/deploy layout), then the
    // system PATH.
    static QString resolveFfmpegPath();

    // Compiles an AnimatedProperty into an FFmpeg expression in `timeVar`, which
    // is the filter's own notion of current time ("t" for overlay/scale, "T" for
    // geq). `clipStartSec` shifts timeline time into clip-relative time so the
    // keyframes mean the same thing they do in the preview.
    //
    // Static properties compile to a bare number, which matters for more than
    // tidiness: it lets the caller skip entire filters when nothing is animated.
    static QString compileExpression(const AnimatedProperty& prop, double clipStartSec,
                                     const QString& timeVar, double multiplier = 1.0);

    // Builds the complete filter graph plus the input arguments it refers to.
    // Kept separate from execution so it can be tested without rendering.
    static QString buildFilterGraph(const Project& project, const Options& options,
                                    QStringList* inputArgs, bool* hasAudio);

private:
    QString m_error;
    QString m_lastGraph;
};
