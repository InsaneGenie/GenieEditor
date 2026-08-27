#pragma once

#include <QString>
#include <QVector>
#include <functional>
#include "Project.h"

// Opaque forward declarations — just enough to name pointer types in the
// progress callback signature below, without pulling whisper.h into this
// header (keeping the "zero whisper.cpp types in the public header" goal).
struct whisper_context;
struct whisper_state;

// Wraps whisper.cpp to produce timestamped transcript segments from a
// media file's audio track. Runs locally, no network/cloud dependency —
// matches the rest of the app's "no external service" design.
//
// whisper.cpp is fetched/built via CMake FetchContent (see CMakeLists.txt).
// A GGML model file is a separate download this doesn't handle — get one
// from https://huggingface.co/ggerganov/whisper.cpp/tree/main
// (ggml-base.en.bin is a solid starting point) — MainWindow resolves the
// model path (checks a conventional location, else prompts once and
// remembers the choice) before constructing this.
//
// This header intentionally has zero whisper.cpp types in it so the rest
// of the app doesn't need whisper's headers on the include path — only
// Transcriber.cpp does.
class Transcriber {
public:
    explicit Transcriber(const QString& modelPath);
    ~Transcriber();

    // Extracts audio from mediaPath (via FFmpeg, resampled to 16kHz mono —
    // whisper's required input format) and runs it through whisper.cpp.
    // Blocking call; run this on a worker thread in the UI.
    QVector<TranscriptSegment> transcribe(const QString& mediaPath);

    bool isModelLoaded() const;
    const QString& errorMessage() const { return m_error; }

    // Reports 0-100 periodically DURING the whisper inference phase of
    // transcribe() (not during the FFmpeg audio-decode phase beforehand,
    // which whisper.cpp has no visibility into — the bar will sit at 0
    // briefly before inference actually starts). Invoked from WHATEVER
    // thread transcribe() runs on; this does not marshal to the GUI thread
    // itself — the caller must do that (e.g. via QPromise::setProgressValue
    // from within a QtConcurrent worker, which is thread-safe to call from
    // any thread).
    void setProgressCallback(std::function<void(int)> callback);

private:
    static void progressTrampoline(whisper_context* ctx, whisper_state* state, int progress, void* userData);

    struct Impl;
    Impl* m_impl = nullptr;
    QString m_error;
    std::function<void(int)> m_progressCallback;
};
