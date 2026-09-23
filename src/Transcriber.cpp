#include "Transcriber.h"
#include "whisper.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <vector>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <chrono>

namespace {

constexpr int kWhisperSampleRate = 16000;

// Streams a file's best audio stream as mono 16kHz float32 PCM — whisper.cpp's
// required input format — a piece at a time.
//
// This used to decode the WHOLE file into one vector before transcribing,
// which is ~230 MB per hour of audio. At 80 hours that's ~18 GB, and past
// ~37 hours the sample count no longer fits the int whisper_full takes, so
// the call receives a garbage length. Streaming bounds memory to one window
// (see Transcriber::transcribe) no matter how long the file is.
//
// Same swr_alloc_set_opts2 pattern as WaveformGenerator, resampling to a fixed
// 16000 Hz instead of the source rate.
class PcmStream {
public:
    ~PcmStream() { close(); }

    bool open(const QString& path) {
        const QByteArray pathUtf8 = path.toUtf8();
        if (avformat_open_input(&m_fmt, pathUtf8.constData(), nullptr, nullptr) != 0) return false;
        if (avformat_find_stream_info(m_fmt, nullptr) < 0) return false;

        const AVCodec* decoder = nullptr;
        m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (m_stream < 0 || !decoder) return false; // no audio stream — nothing to transcribe

        // Video packets are dropped at the demuxer, so an 80-hour screen
        // recording isn't read off disk twice as heavily as it needs to be.
        for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
            if (static_cast<int>(i) != m_stream) m_fmt->streams[i]->discard = AVDISCARD_ALL;
        }

        m_codec = avcodec_alloc_context3(decoder);
        if (!m_codec
            || avcodec_parameters_to_context(m_codec, m_fmt->streams[m_stream]->codecpar) < 0
            || avcodec_open2(m_codec, decoder, nullptr) < 0) {
            return false;
        }

        av_channel_layout_default(&m_mono, 1);
        m_monoInit = true;
        if (swr_alloc_set_opts2(&m_swr, &m_mono, AV_SAMPLE_FMT_FLT, kWhisperSampleRate,
                                &m_codec->ch_layout, m_codec->sample_fmt, m_codec->sample_rate,
                                0, nullptr) < 0
            || !m_swr || swr_init(m_swr) < 0) {
            return false;
        }

        m_packet = av_packet_alloc();
        m_frame = av_frame_alloc();

        if (m_fmt->duration != AV_NOPTS_VALUE) {
            m_totalSamples = static_cast<int64_t>(double(m_fmt->duration) / AV_TIME_BASE * kWhisperSampleRate);
        }
        return m_packet && m_frame;
    }

    // Best-effort estimate for progress reporting; 0 when the container
    // doesn't say.
    int64_t totalSamples() const { return m_totalSamples; }
    bool atEnd() const { return m_finished; }

    // Appends decoded samples to `out` until it holds at least `target`, or the
    // file runs out.
    void fill(std::vector<float>& out, size_t target) {
        while (out.size() < target && !m_finished) {
            if (av_read_frame(m_fmt, m_packet) >= 0) {
                if (m_packet->stream_index == m_stream && avcodec_send_packet(m_codec, m_packet) == 0) {
                    receiveFrames(out);
                }
                av_packet_unref(m_packet);
                continue;
            }

            // End of file: drain frames the decoder still holds, then whatever
            // the resampler has buffered, then stop for good.
            if (avcodec_send_packet(m_codec, nullptr) == 0) receiveFrames(out);
            int flushed;
            do {
                const int outSamples = std::max(swr_get_out_samples(m_swr, 0), 4096);
                if (outSamples > static_cast<int>(m_conv.size())) m_conv.resize(outSamples);
                uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(m_conv.data()) };
                flushed = swr_convert(m_swr, outPtrs, static_cast<int>(m_conv.size()), nullptr, 0);
                if (flushed > 0) out.insert(out.end(), m_conv.begin(), m_conv.begin() + flushed);
            } while (flushed > 0);
            m_finished = true;
        }
    }

private:
    void receiveFrames(std::vector<float>& out) {
        while (avcodec_receive_frame(m_codec, m_frame) == 0) {
            const int outSamples = swr_get_out_samples(m_swr, m_frame->nb_samples);
            if (outSamples > static_cast<int>(m_conv.size())) m_conv.resize(outSamples);
            uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(m_conv.data()) };
            const int converted = swr_convert(m_swr, outPtrs, outSamples,
                const_cast<const uint8_t**>(m_frame->data), m_frame->nb_samples);
            if (converted > 0) out.insert(out.end(), m_conv.begin(), m_conv.begin() + converted);
        }
    }

    void close() {
        if (m_frame) av_frame_free(&m_frame);
        if (m_packet) av_packet_free(&m_packet);
        if (m_swr) swr_free(&m_swr);
        if (m_monoInit) av_channel_layout_uninit(&m_mono);
        if (m_codec) avcodec_free_context(&m_codec);
        if (m_fmt) avformat_close_input(&m_fmt);
    }

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_codec = nullptr;
    SwrContext* m_swr = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    AVChannelLayout m_mono{};
    bool m_monoInit = false;
    int m_stream = -1;
    bool m_finished = false;
    int64_t m_totalSamples = 0;
    std::vector<float> m_conv;
};

bool abortTrampoline(void* userData) {
    const auto* check = static_cast<const std::function<bool()>*>(userData);
    return check && *check && (*check)();
}

} // namespace

struct Transcriber::Impl {
    QString modelPath;
    whisper_context* ctx = nullptr;
    bool loaded = false;
};

Transcriber::Transcriber(const QString& modelPath) {
    m_impl = new Impl();
    m_impl->modelPath = modelPath;

    // use_gpu defaults to true already, but set explicitly so the intent
    // is clear in code: if this binary was built with GGML_VULKAN (see
    // CMakeLists.txt), whisper.cpp will automatically use it; on a
    // CPU-only build this flag is simply a no-op.
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    m_impl->ctx = whisper_init_from_file_with_params(modelPath.toUtf8().constData(), cparams);
    if (!m_impl->ctx) {
        m_error = "Failed to load whisper model at \"" + modelPath
                + "\" — check the file exists and is a valid GGML model.";
        return;
    }
    m_impl->loaded = true;
}

Transcriber::~Transcriber() {
    if (m_impl) {
        if (m_impl->ctx) whisper_free(m_impl->ctx);
        delete m_impl;
    }
}

bool Transcriber::isModelLoaded() const {
    return m_impl && m_impl->loaded;
}

void Transcriber::setProgressCallback(std::function<void(int)> callback) {
    m_progressCallback = std::move(callback);
}

void Transcriber::setCancelCheck(std::function<bool()> check) {
    m_cancelCheck = std::move(check);
}

void Transcriber::setPauseCheck(std::function<bool()> check) {
    m_pauseCheck = std::move(check);
}

void Transcriber::setThreadCount(int threads) {
    m_threadCount = threads;
}

void Transcriber::progressTrampoline(whisper_context*, whisper_state*, int progress, void* userData) {
    auto* self = static_cast<Transcriber*>(userData);
    if (!self || !self->m_progressCallback) return;

    // whisper reports 0-100 for the WINDOW it's working on. Rescaled to the
    // whole file, so the bar moves steadily across an 80-hour job instead of
    // sweeping 0-100 hundreds of times.
    if (self->m_totalSamples > 0) {
        const double done = double(self->m_windowStartSample)
                          + double(self->m_windowSamples) * (progress / 100.0);
        self->m_progressCallback(std::clamp(static_cast<int>(done / self->m_totalSamples * 100.0), 0, 100));
    } else {
        self->m_progressCallback(progress);
    }
}

QVector<TranscriptSegment> Transcriber::transcribe(const QString& mediaPath) {
    QVector<TranscriptSegment> result;
    if (!isModelLoaded()) return result;

    PcmStream stream;
    if (!stream.open(mediaPath)) {
        m_error = "Couldn't decode any audio from this file.";
        return result;
    }

    // --- Windowing ---------------------------------------------------------
    // The file is fed to whisper in ~10-minute windows, each overlapping the
    // next by 30 seconds. Only segments that START inside the first
    // kAdvanceSec of a window are kept; the overlap exists so the segment
    // that straddles that boundary is heard in full rather than cut mid-word.
    //
    // The next window then begins where the last kept segment ENDED, not at a
    // fixed stride. That's what makes the seams clean: nothing kept is heard
    // twice, and nothing between two kept segments falls through the gap.
    //
    // 30 s of overlap because whisper's own segments top out around 30 s — a
    // line starting just before the boundary is guaranteed to finish inside
    // the window.
    constexpr double kAdvanceSec = 600.0;
    constexpr double kOverlapSec = 30.0;
    constexpr size_t kWindowSamples = size_t((kAdvanceSec + kOverlapSec) * kWhisperSampleRate);
    constexpr int64_t kAdvanceCs = int64_t(kAdvanceSec * 100); // whisper timestamps are centiseconds
    // The least a window ever advances. Nothing kept starts after the
    // boundary, so everything up to it is either already transcribed or
    // silence — only the last kOverlapSec before it is worth hearing again,
    // for a line whisper might time slightly differently the second time.
    constexpr size_t kMinAdvanceSamples = size_t((kAdvanceSec - kOverlapSec) * kWhisperSampleRate);
    // Tails shorter than this are silence or a click; whisper will cheerfully
    // hallucinate a sentence into half a second of nothing.
    constexpr size_t kMinWindowSamples = kWhisperSampleRate / 2;

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_special = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.token_timestamps = true; // required for the word-level timing below

    // Disables using the PREVIOUS segment's (possibly wrong) text as
    // decoding context for the next one. This is specifically to prevent
    // hallucination loops — a well-known Whisper failure mode on
    // silent/unclear audio where one bad guess gets fed back in as context,
    // reinforcing the same wrong phrase again, cascading into the same
    // line repeating for minutes (e.g. "I'm not sure." over and over).
    // Without this, each segment decodes more independently, so one bad
    // guess doesn't poison everything after it.
    params.no_context = true;

    // Leave a little headroom for playback/UI/background decoding instead of
    // saturating every logical core and making the editor feel frozen.
    const unsigned hwThreads = std::thread::hardware_concurrency();
    params.n_threads = hwThreads > 2 ? static_cast<int>(hwThreads - 2)
                                     : (hwThreads > 0 ? static_cast<int>(hwThreads) : 4);
    if (m_threadCount > 0) params.n_threads = m_threadCount;

    params.progress_callback = &Transcriber::progressTrampoline;
    params.progress_callback_user_data = this;
    if (m_cancelCheck) {
        params.abort_callback = &abortTrampoline;
        params.abort_callback_user_data = &m_cancelCheck;
    }

    m_totalSamples = stream.totalSamples();
    std::vector<float> window;
    window.reserve(kWindowSamples + 8192);
    int64_t windowStartSample = 0; // absolute position of window[0] in the file
    bool decodedAnything = false;

    while (true) {
        if (m_cancelCheck && m_cancelCheck()) return {};

        // Yield between windows while asked to. Checked here rather than
        // inside whisper, which can't be suspended mid-inference — so a pause
        // takes effect within one window's worth of work (well under a minute
        // on most machines) rather than instantly.
        while (m_pauseCheck && m_pauseCheck()) {
            if (m_cancelCheck && m_cancelCheck()) return {};
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        stream.fill(window, kWindowSamples);
        if (!window.empty()) decodedAnything = true;
        if (window.size() < kMinWindowSamples) break;

        // The last window is the one that holds everything left. Its segments
        // are all kept — there's no next window to hand the tail to.
        const bool isLast = stream.atEnd() && window.size() <= kWindowSamples;
        const int n = static_cast<int>(std::min(window.size(), kWindowSamples));

        m_windowStartSample = windowStartSample;
        m_windowSamples = n;

        if (whisper_full(m_impl->ctx, params, window.data(), n) != 0) {
            if (m_cancelCheck && m_cancelCheck()) return {}; // aborted, not failed
            m_error = "Whisper transcription failed.";
            return result;
        }

        const double offsetSec = double(windowStartSample) / kWhisperSampleRate;
        int64_t lastKeptEndCs = -1;

        const int nSegments = whisper_full_n_segments(m_impl->ctx);
        for (int i = 0; i < nSegments; ++i) {
            const int64_t t0 = whisper_full_get_segment_t0(m_impl->ctx, i);
            const int64_t t1 = whisper_full_get_segment_t1(m_impl->ctx, i);
            if (!isLast && t0 >= kAdvanceCs) break; // belongs to the next window

            TranscriptSegment seg;
            seg.text = QString::fromUtf8(whisper_full_get_segment_text(m_impl->ctx, i)).trimmed();
            seg.startSec = offsetSec + t0 / 100.0;
            seg.endSec = offsetSec + t1 / 100.0;

            // whisper.cpp tokens are often BPE subword pieces, not whole words —
            // a token that STARTS a new word is conventionally decoded with a
            // leading space (" Hello" vs a continuation piece like "llo" with
            // none). Tokens without a leading space are merged into the
            // previous word rather than shown as their own separate, confusing
            // fragment in the transcript.
            const int nTokens = whisper_full_n_tokens(m_impl->ctx, i);
            for (int t = 0; t < nTokens; ++t) {
                const QString tokenText = QString::fromUtf8(whisper_full_get_token_text(m_impl->ctx, i, t));
                if (tokenText.startsWith('[') || tokenText.trimmed().isEmpty()) {
                    continue; // special/control tokens (e.g. timestamp markers) — not real text
                }

                const whisper_token_data token = whisper_full_get_token_data(m_impl->ctx, i, t);
                const bool startsNewWord = tokenText.startsWith(' ') || seg.words.isEmpty();

                if (startsNewWord) {
                    TranscriptWord word;
                    word.text = tokenText.trimmed();
                    word.startSec = offsetSec + token.t0 / 100.0;
                    word.endSec = offsetSec + token.t1 / 100.0;
                    seg.words.push_back(word);
                } else {
                    TranscriptWord& prev = seg.words.back();
                    prev.text += tokenText;
                    prev.endSec = offsetSec + token.t1 / 100.0;
                }
            }

            result.push_back(seg);
            lastKeptEndCs = std::max(lastKeptEndCs, t1);
        }

        if (isLast) break;

        // Advance to where the last kept line ended, but never less than the
        // minimum stride (that keeps silent stretches moving) and never past
        // the audio this window actually heard.
        size_t advance = lastKeptEndCs >= 0 ? size_t(lastKeptEndCs) * kWhisperSampleRate / 100 : 0;
        advance = std::clamp(advance, kMinAdvanceSamples, size_t(n));

        window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(advance));
        windowStartSample += static_cast<int64_t>(advance);
    }

    if (!decodedAnything) {
        m_error = "Couldn't decode any audio from this file.";
        return result;
    }
    if (m_progressCallback) m_progressCallback(100);
    return result;
}
