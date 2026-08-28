#include "WaveformGenerator.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QFileInfo>
#include <QDateTime>
#include <limits>
#include <vector>
#include <cmath>
#include <algorithm>

namespace {
// Buckets per second of audio when the caller doesn't specify a count. 200 is
// one measurement every 5ms, which at the timeline's maximum zoom (800 px/s)
// still leaves four pixels per bucket — fine enough that interpolating between
// them draws a curve instead of a staircase.
constexpr double kBucketsPerSecond = 200.0;

// A floor for very short files, so a two-second clip still gets a dense
// waveform rather than four hundred buckets.
constexpr int kMinBuckets = 2000;

// And a ceiling, because this is held in memory for as long as the clip exists.
// 400k buckets is ~3.2MB across both arrays; past roughly 33 minutes a file
// loses resolution gradually rather than growing without bound.
constexpr int kMaxBuckets = 400000;
} // namespace

namespace {

// Decoded waveforms, keyed by file identity rather than path alone.
//
// Generating one means decoding the ENTIRE audio stream, which is by far the
// most expensive thing this application does on import. It was also being done
// more than once for the same audio: a video clip and its companion audio clip
// share a source, splitting a clip gives both halves the same source, and
// re-importing a file starts from scratch again. Every one of those decoded the
// same samples over again, in parallel, competing with playback.
//
// Keyed on modification time and size as well as path, so re-exporting a file
// and reopening the project picks up the new audio rather than a stale cache.
struct WaveformCacheEntry {
    WaveformData data;
    qint64 modifiedMs = 0;
    qint64 sizeBytes = 0;
    int peakCount = 0;
    quint64 lastUsed = 0;
};

QHash<QString, WaveformCacheEntry> g_waveformCache;
QMutex g_waveformCacheMutex;
quint64 g_waveformUseCounter = 0;

// The arrays are large, so the cache is bounded and drops least-recently-used
// entries. Exceeding it costs a re-decode, never a wrong result.
constexpr int kMaxCachedWaveforms = 12;

void trimWaveformCache() {
    while (g_waveformCache.size() > kMaxCachedWaveforms) {
        QString oldestKey;
        quint64 oldest = std::numeric_limits<quint64>::max();
        for (auto it = g_waveformCache.constBegin(); it != g_waveformCache.constEnd(); ++it) {
            if (it->lastUsed < oldest) { oldest = it->lastUsed; oldestKey = it.key(); }
        }
        if (oldestKey.isEmpty()) break;
        g_waveformCache.remove(oldestKey);
    }
}

} // namespace

void WaveformGenerator::clearCache() {
    QMutexLocker lock(&g_waveformCacheMutex);
    g_waveformCache.clear();
}

WaveformData WaveformGenerator::generate(const QString& path, int peakCount) {
    WaveformData result;

    // Captured BEFORE the resolution logic below overwrites peakCount with the
    // count it actually chose. Storing the resolved value while looking up the
    // requested one means the key never matches and the cache silently never
    // hits -- which looks exactly like it working, only slowly.
    const int requestedPeakCount = peakCount;

    const QFileInfo probeInfo(path);
    const qint64 modifiedMs = probeInfo.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = probeInfo.size();
    {
        QMutexLocker lock(&g_waveformCacheMutex);
        auto cached = g_waveformCache.find(path);
        if (cached != g_waveformCache.end()
            && cached->modifiedMs == modifiedMs
            && cached->sizeBytes == sizeBytes
            && cached->peakCount == requestedPeakCount) {
            cached->lastUsed = ++g_waveformUseCounter;
            // QVector is implicitly shared, so this hands back a refcount bump
            // rather than copying several megabytes of samples.
            return cached->data;
        }
    }

    AVFormatContext* fmtCtx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        return result;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    const AVCodec* decoder = nullptr;
    const int audioStreamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStreamIndex < 0 || !decoder) {
        avformat_close_input(&fmtCtx);
        return result; // no audio stream — nothing to visualize, not an error
    }

    AVStream* stream = fmtCtx->streams[audioStreamIndex];

    double durationSec = 0.0;
    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        durationSec = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
    } else if (stream->duration != AV_NOPTS_VALUE) {
        durationSec = stream->duration * av_q2d(stream->time_base);
    }
    if (durationSec <= 0.0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Resolution is chosen here rather than by the caller because it depends on
    // the duration, which isn't known until the container has been opened.
    if (peakCount <= 0) {
        peakCount = std::clamp(static_cast<int>(durationSec * kBucketsPerSecond),
                               kMinBuckets, kMaxBuckets);
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx
        || avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0
        || avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Resample/downmix to mono float — only amplitude matters here, not the
    // original channel layout or sample rate.
    SwrContext* swr = nullptr;
    AVChannelLayout monoLayout;
    av_channel_layout_default(&monoLayout, 1);
    int swrRet = swr_alloc_set_opts2(&swr,
        &monoLayout, AV_SAMPLE_FMT_FLT, codecCtx->sample_rate,
        &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
        0, nullptr);
    if (swrRet < 0 || !swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Estimate total mono samples across the whole file up front, so each
    // downsample "bucket" can be sized to hit exactly `peakCount` buckets
    // by the end, regardless of file length.
    const double totalSamplesEstimate = durationSec * codecCtx->sample_rate;
    const double samplesPerBucket = std::max(1.0, totalSamplesEstimate / peakCount);

    result.peaks.reserve(peakCount + 1);
    result.rms.reserve(peakCount + 1);
    double bucketMax = 0.0;      // loudest sample seen in the current bucket
    double bucketEnergy = 0.0;   // running sum of squares, for the RMS
    int bucketSamples = 0;
    double sampleCounter = 0.0;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> monoBuf;

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index == audioStreamIndex
            && avcodec_send_packet(codecCtx, packet) == 0) {
            while (avcodec_receive_frame(codecCtx, frame) == 0) {
                const int outSamples = swr_get_out_samples(swr, frame->nb_samples);
                if (outSamples > static_cast<int>(monoBuf.size())) {
                    monoBuf.resize(outSamples);
                }
                uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(monoBuf.data()) };
                const int converted = swr_convert(swr, outPtrs, outSamples,
                    const_cast<const uint8_t**>(frame->data), frame->nb_samples);

                for (int i = 0; i < converted; ++i) {
                    const double sample = monoBuf[i];
                    bucketMax = std::max(bucketMax, std::fabs(sample));
                    bucketEnergy += sample * sample;
                    ++bucketSamples;

                    sampleCounter += 1.0;
                    if (sampleCounter >= samplesPerBucket) {
                        result.peaks.push_back(static_cast<float>(bucketMax));
                        result.rms.push_back(static_cast<float>(
                            std::sqrt(bucketEnergy / bucketSamples)));
                        bucketMax = 0.0;
                        bucketEnergy = 0.0;
                        bucketSamples = 0;
                        // Subtracting rather than zeroing keeps fractional
                        // bucket sizes honest — samplesPerBucket is rarely a
                        // whole number, and zeroing rounds every bucket up,
                        // which compounds into a real length error by the end.
                        sampleCounter -= samplesPerBucket;
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    if (bucketSamples > 0) {
        result.peaks.push_back(static_cast<float>(bucketMax));
        result.rms.push_back(static_cast<float>(std::sqrt(bucketEnergy / bucketSamples)));
    }

    if (!result.peaks.isEmpty()) {
        QMutexLocker lock(&g_waveformCacheMutex);
        g_waveformCache.insert(path, WaveformCacheEntry{
            result, modifiedMs, sizeBytes, requestedPeakCount, ++g_waveformUseCounter});
        trimWaveformCache();
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    result.durationSec = durationSec;
    return result;
}
