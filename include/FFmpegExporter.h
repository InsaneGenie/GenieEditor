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
    using ProgressFn = std::function<bool(double)>;

    bool exportProject(const Project& project, const Options& options, ProgressFn onProgress = {});

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
