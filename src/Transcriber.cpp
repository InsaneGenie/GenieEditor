#include "Transcriber.h"
#include "whisper.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <vector>
#include <thread>

namespace {
// Decodes the best audio stream of `path` fully into mono 16kHz float32
// PCM — whisper.cpp's required input format, no file I/O in between.
// Follows the exact same swr_alloc_set_opts2 pattern as WaveformGenerator,
// just resampling to a fixed 16000 Hz instead of the source rate. Returns
// an empty vector on failure (missing file, no audio stream, decode error).
std::vector<float> decodeAudioToPcm16kMono(const QString& path) {
    std::vector<float> samples;

    AVFormatContext* fmtCtx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        return samples;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return samples;
    }

    const AVCodec* decoder = nullptr;
    const int audioStreamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStreamIndex < 0 || !decoder) {
        avformat_close_input(&fmtCtx);
        return samples; // no audio stream — nothing to transcribe
    }

    AVStream* stream = fmtCtx->streams[audioStreamIndex];
    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx
        || avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0
        || avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return samples;
    }

    constexpr int kWhisperSampleRate = 16000;
    SwrContext* swr = nullptr;
    AVChannelLayout monoLayout;
    av_channel_layout_default(&monoLayout, 1);
    const int swrRet = swr_alloc_set_opts2(&swr,
        &monoLayout, AV_SAMPLE_FMT_FLT, kWhisperSampleRate,
        &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
        0, nullptr);
    if (swrRet < 0 || !swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return samples;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> convBuf;

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index == audioStreamIndex
            && avcodec_send_packet(codecCtx, packet) == 0) {
            while (avcodec_receive_frame(codecCtx, frame) == 0) {
                const int outSamples = swr_get_out_samples(swr, frame->nb_samples);
                if (outSamples > static_cast<int>(convBuf.size())) {
                    convBuf.resize(outSamples);
                }
                uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(convBuf.data()) };
                const int converted = swr_convert(swr, outPtrs, outSamples,
                    const_cast<const uint8_t**>(frame->data), frame->nb_samples);
                if (converted > 0) {
                    samples.insert(samples.end(), convBuf.begin(), convBuf.begin() + converted);
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush any samples still buffered inside the resampler.
    int flushed;
    do {
        const int outSamples = swr_get_out_samples(swr, 0);
        if (outSamples > static_cast<int>(convBuf.size())) convBuf.resize(std::max(outSamples, 4096));
        uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(convBuf.data()) };
        flushed = swr_convert(swr, outPtrs, static_cast<int>(convBuf.size()), nullptr, 0);
        if (flushed > 0) samples.insert(samples.end(), convBuf.begin(), convBuf.begin() + flushed);
    } while (flushed > 0);

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    av_channel_layout_uninit(&monoLayout);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return samples;
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

void Transcriber::progressTrampoline(whisper_context*, whisper_state*, int progress, void* userData) {
    auto* self = static_cast<Transcriber*>(userData);
    if (self && self->m_progressCallback) {
        self->m_progressCallback(progress);
    }
}

QVector<TranscriptSegment> Transcriber::transcribe(const QString& mediaPath) {
    QVector<TranscriptSegment> result;
    if (!isModelLoaded()) return result;

    const std::vector<float> samples = decodeAudioToPcm16kMono(mediaPath);
    if (samples.empty()) {
        m_error = "Couldn't decode any audio from this file.";
        return result;
    }

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

    // whisper.cpp's own default thread count is fairly conservative —
    // explicitly use every logical core available rather than leaving
    // real speedup on the table on a multi-core machine.
    const unsigned hwThreads = std::thread::hardware_concurrency();
    params.n_threads = hwThreads > 0 ? static_cast<int>(hwThreads) : 4;

    params.progress_callback = &Transcriber::progressTrampoline;
    params.progress_callback_user_data = this;

    if (whisper_full(m_impl->ctx, params, samples.data(), static_cast<int>(samples.size())) != 0) {
        m_error = "Whisper transcription failed.";
        return result;
    }

    const int nSegments = whisper_full_n_segments(m_impl->ctx);
    result.reserve(nSegments);

    for (int i = 0; i < nSegments; ++i) {
        TranscriptSegment seg;
        seg.text = QString::fromUtf8(whisper_full_get_segment_text(m_impl->ctx, i)).trimmed();
        seg.startSec = whisper_full_get_segment_t0(m_impl->ctx, i) / 100.0; // whisper reports centiseconds
        seg.endSec = whisper_full_get_segment_t1(m_impl->ctx, i) / 100.0;

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
                word.startSec = token.t0 / 100.0;
                word.endSec = token.t1 / 100.0;
                seg.words.push_back(word);
            } else {
                TranscriptWord& prev = seg.words.back();
                prev.text += tokenText;
                prev.endSec = token.t1 / 100.0;
            }
        }

        result.push_back(seg);
    }

    return result;
}
