#include "MotionAnalyzer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace {

// Analysis resolution. Small on purpose: motion detection wants "did the
// picture change", not detail, and downscaling with area averaging is itself a
// denoiser — grain and compression shimmer average out before they're
// measured. 160x90 keeps a mouse cursor at roughly one pixel, which is what
// lets the threshold treat it as the non-event it is.
constexpr int kW = 160;
constexpr int kH = 90;

// How far a pixel's brightness has to move (0-255) to count as changed.
// Below this is noise; above it is something a viewer would notice.
constexpr int kPixelDelta = 14;

struct CacheEntry {
    MotionProfile profile;
    qint64 modifiedMs = 0;
    qint64 sizeBytes = 0;
};

QHash<QString, CacheEntry> g_cache;
QMutex g_cacheMutex;

bool lookupCache(const QString& path, MotionProfile* out) {
    const QFileInfo info(path);
    QMutexLocker lock(&g_cacheMutex);
    auto it = g_cache.constFind(path);
    if (it == g_cache.constEnd()) return false;
    if (it->modifiedMs != info.lastModified().toMSecsSinceEpoch() || it->sizeBytes != info.size())
        return false;
    if (out) *out = it->profile;
    return true;
}

} // namespace

float MotionProfile::maxChangeBetween(double fromSec, double toSec) const {
    if (!isValid()) return 1.0f;
    if (toSec < fromSec) std::swap(fromSec, toSec);
    const int n = changedFraction.size();
    const int a = std::clamp(static_cast<int>(std::floor(fromSec / intervalSec)), 0, n - 1);
    // +1 so the sample that closes the window — the change INTO the frame at
    // toSec — is included, not just the ones strictly inside it.
    const int b = std::clamp(static_cast<int>(std::floor(toSec / intervalSec)) + 1, 0, n - 1);
    float m = 0.0f;
    for (int i = a; i <= b; ++i) m = std::max(m, changedFraction[i]);
    return m;
}

bool MotionAnalyzer::isCached(const QString& path) {
    return lookupCache(path, nullptr);
}

void MotionAnalyzer::clearCache() {
    QMutexLocker lock(&g_cacheMutex);
    g_cache.clear();
}

MotionProfile MotionAnalyzer::analyze(const QString& path, const std::atomic_bool* cancel,
                                      const ProgressFn& progress, double samplesPerSec) {
    MotionProfile cached;
    if (lookupCache(path, &cached)) {
        if (progress) progress(1.0);
        return cached;
    }

    MotionProfile result;
    result.intervalSec = 1.0 / std::max(0.5, samplesPerSec);

    AVFormatContext* fmtCtx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) != 0) return result;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    const AVCodec* decoder = nullptr;
    const int streamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (streamIndex < 0 || !decoder) {
        avformat_close_input(&fmtCtx);
        return result; // audio-only: nothing to measure, not an error
    }
    AVStream* stream = fmtCtx->streams[streamIndex];

    // Everything else in the container is skipped at the demuxer, so audio and
    // subtitle packets are never even read into memory.
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (static_cast<int>(i) != streamIndex) fmtCtx->streams[i]->discard = AVDISCARD_ALL;
    }

    double durationSec = 0.0;
    if (fmtCtx->duration != AV_NOPTS_VALUE) durationSec = double(fmtCtx->duration) / AV_TIME_BASE;
    else if (stream->duration != AV_NOPTS_VALUE) durationSec = stream->duration * av_q2d(stream->time_base);
    if (durationSec <= 0.0) {
        avformat_close_input(&fmtCtx);
        return result;
    }
    const double streamStartSec = (stream->start_time != AV_NOPTS_VALUE)
        ? stream->start_time * av_q2d(stream->time_base) : 0.0;

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx || avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }
    // Speed over fidelity everywhere it's free. This is a full sequential
    // decode of the file, so it dominates the cost of the whole feature:
    //  - all cores, since nothing else is decoding while the dialog is up;
    //  - skip non-reference frames (B-frames on most H.264/HEVC), which are
    //    typically half or more of the stream and aren't needed at 4 samples
    //    a second;
    //  - skip the deblocking filter, whose only job is hiding block edges that
    //    160x90 downscaling erases anyway.
    codecCtx->thread_count = 0;
    codecCtx->skip_frame = AVDISCARD_NONREF;
    codecCtx->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    SwsContext* sws = nullptr; // created lazily: some decoders only report pix_fmt after the first frame
    std::vector<uint8_t> prev(kW * kH), cur(kW * kH);
    bool havePrev = false;

    const int sampleCount = static_cast<int>(std::ceil(durationSec / result.intervalSec)) + 1;
    // -1 marks "no frame landed in this slot yet"; filled from neighbours after.
    QVector<float> samples(sampleCount, -1.0f);
    int lastFilledSlot = -1;
    double lastReported = -1.0;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool cancelled = false;

    auto handleFrame = [&](AVFrame* f) {
        const int64_t rawPts = (f->pts != AV_NOPTS_VALUE) ? f->pts : f->best_effort_timestamp;
        if (rawPts == AV_NOPTS_VALUE) return;
        const double t = rawPts * av_q2d(stream->time_base) - streamStartSec;
        const int slot = std::clamp(static_cast<int>(std::floor(t / result.intervalSec)), 0, sampleCount - 1);
        if (slot <= lastFilledSlot) return; // already sampled this moment; skip the scale entirely

        if (!sws) {
            sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                 kW, kH, AV_PIX_FMT_GRAY8, SWS_AREA, nullptr, nullptr, nullptr);
            if (!sws) return;
        }
        uint8_t* dst[4] = {cur.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {kW, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstStride);

        if (havePrev) {
            int changed = 0;
            for (int i = 0; i < kW * kH; ++i) {
                if (std::abs(int(cur[i]) - int(prev[i])) > kPixelDelta) ++changed;
            }
            samples[slot] = float(changed) / float(kW * kH);
        }
        std::swap(prev, cur);
        havePrev = true;
        lastFilledSlot = slot;

        if (progress) {
            const double frac = std::clamp(t / durationSec, 0.0, 1.0);
            if (frac - lastReported >= 0.005) {
                lastReported = frac;
                progress(frac);
            }
        }
    };

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (cancel && cancel->load()) { cancelled = true; av_packet_unref(packet); break; }
        if (packet->stream_index == streamIndex && avcodec_send_packet(codecCtx, packet) == 0) {
            while (avcodec_receive_frame(codecCtx, frame) == 0) handleFrame(frame);
        }
        av_packet_unref(packet);
    }
    if (!cancelled) {
        avcodec_send_packet(codecCtx, nullptr); // drain frames still held for reordering
        while (avcodec_receive_frame(codecCtx, frame) == 0) handleFrame(frame);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (cancelled || lastFilledSlot < 0) return MotionProfile{};

    // Fill slots no frame landed in (variable frame rate, dropped frames, the
    // very first sample which has nothing to diff against) from the next
    // reading after them, and the tail from the last one. Borrowing a real
    // neighbour's value is always closer to the truth than assuming "still".
    float next = -1.0f;
    for (int i = sampleCount - 1; i >= 0; --i) {
        if (samples[i] >= 0.0f) next = samples[i];
        else if (next >= 0.0f) samples[i] = next;
    }
    float last = 0.0f;
    for (int i = 0; i < sampleCount; ++i) {
        if (samples[i] >= 0.0f) last = samples[i];
        else samples[i] = last;
    }

    result.changedFraction = samples;
    result.durationSec = durationSec;

    const QFileInfo info(path);
    {
        QMutexLocker lock(&g_cacheMutex);
        g_cache.insert(path, CacheEntry{result, info.lastModified().toMSecsSinceEpoch(), info.size()});
    }
    if (progress) progress(1.0);
    return result;
}

QVector<DowntimeRegion> MotionAnalyzer::findDowntime(const Project& project,
                                                     const QHash<QString, MotionProfile>& profiles,
                                                     const DowntimeSettings& settings) {
    QVector<DowntimeRegion> regions;
    const double projectEnd = project.durationSec();
    if (projectEnd <= 0.0) return regions;

    // The timeline is judged in fixed cells. 0.1s is well under the sample
    // spacing, so it never limits accuracy, and it keeps a multi-hour project
    // to a few hundred thousand cells.
    constexpr double kCell = 0.1;
    const int cellCount = static_cast<int>(std::ceil(projectEnd / kCell));
    std::vector<char> hasVideo(cellCount, 0), moving(cellCount, 0), loud(cellCount, 0);
    const float threshold = float(settings.stillThresholdPct / 100.0);

    // Driven by the clips rather than the cells: each clip only visits the
    // cells it covers, so the cost scales with how much footage there is, not
    // with (cells x clips).
    auto forEachCoveredCell = [&](const Clip& clip, auto&& fn) {
        const double clipStart = clip.trackPosSec;
        const double clipEnd = clipStart + clip.durationSec();
        const int first = std::max(0, static_cast<int>(std::floor(clipStart / kCell)));
        const int last = std::min(cellCount - 1, static_cast<int>(std::ceil(clipEnd / kCell)));
        for (int i = first; i <= last; ++i) {
            const double t0 = i * kCell;
            const double mid = t0 + kCell * 0.5;
            if (mid < clipStart || mid >= clipEnd) continue;
            // Through the clip's own mapping, so a 4x clip checks four times as
            // much source per cell — which is exactly what's on screen.
            const double s0 = clip.sourceTimeAt(std::max(t0, clipStart));
            const double s1 = clip.sourceTimeAt(std::min(t0 + kCell, clipEnd));
            fn(i, s0, s1);
        }
    };

    for (const Track& track : project.tracks) {
        if (!track.enabled) continue;

        if (track.type == TrackType::Video) {
            for (const Clip& clip : track.clips) {
                const auto it = profiles.constFind(clip.sourcePath);
                const MotionProfile* profile = (it != profiles.constEnd() && it->isValid()) ? &*it : nullptr;
                forEachCoveredCell(clip, [&](int i, double s0, double s1) {
                    hasVideo[i] = 1;
                    // No profile means no evidence either way. Keeping the
                    // footage is the only safe reading of "don't know".
                    if (!profile || profile->maxChangeBetween(s0, s1) > threshold) moving[i] = 1;
                });
            }
        } else if (track.type == TrackType::Audio && settings.requireSilence && !track.muted) {
            for (const Clip& clip : track.clips) {
                const QVector<float>& energy = clip.waveformRms.isEmpty() ? clip.waveformPeaks : clip.waveformRms;
                const double energyDur = clip.waveformSourceDurationSec;
                forEachCoveredCell(clip, [&](int i, double s0, double s1) {
                    if (energy.isEmpty() || energyDur <= 0.0) { loud[i] = 1; return; } // unknown = keep
                    const int n = energy.size();
                    const int a = std::clamp(static_cast<int>(s0 / energyDur * n), 0, n - 1);
                    const int b = std::clamp(static_cast<int>(s1 / energyDur * n), 0, n - 1);
                    for (int k = a; k <= b; ++k) {
                        if (energy[k] > settings.silenceRms) { loud[i] = 1; break; }
                    }
                });
            }
        }
    }

    auto isStill = [&](int i) { return hasVideo[i] && !moving[i] && !loud[i]; };

    for (int i = 0; i < cellCount;) {
        if (!isStill(i)) { ++i; continue; }
        int j = i;
        while (j < cellCount && isStill(j)) ++j;

        double start = i * kCell;
        double end = std::min(projectEnd, j * kCell);
        // No padding against the very start or end of the project: there's no
        // motion on the far side to lead into, so the whole dead stretch goes.
        if (start > 1e-6) start += settings.paddingSec;
        if (end < projectEnd - 1e-6) end -= settings.paddingSec;

        if (end - start >= settings.minDurationSec) regions.push_back({start, end});
        i = j;
    }
    return regions;
}
