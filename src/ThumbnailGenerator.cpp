#include "ThumbnailGenerator.h"
#include "VisualDiskCache.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {
// A small in-memory cache in front of the disk one, for the common case of
// the same strip being asked for repeatedly within a session — several clips
// cut from one file, undo restoring clips, reopening a project.
struct ThumbCacheEntry {
    ThumbnailStrip strip;
    qint64 modifiedMs = 0;
    qint64 sizeBytes = 0;
    quint64 lastUsed = 0;
};
QHash<QString, ThumbCacheEntry> g_thumbCache;
QMutex g_thumbCacheMutex;
quint64 g_thumbUseCounter = 0;
constexpr int kMaxCachedStrips = 24;

QString memKey(const QString& path, int count, int w, int h) {
    return QStringLiteral("%1|%2|%3|%4").arg(path).arg(count).arg(w).arg(h);
}

void rememberStrip(const QString& key, const ThumbnailStrip& strip, qint64 modifiedMs, qint64 sizeBytes) {
    QMutexLocker lock(&g_thumbCacheMutex);
    g_thumbCache.insert(key, ThumbCacheEntry{strip, modifiedMs, sizeBytes, ++g_thumbUseCounter});
    while (g_thumbCache.size() > kMaxCachedStrips) {
        auto oldest = g_thumbCache.begin();
        for (auto it = g_thumbCache.begin(); it != g_thumbCache.end(); ++it) {
            if (it->lastUsed < oldest->lastUsed) oldest = it;
        }
        g_thumbCache.erase(oldest);
    }
}
} // namespace

ThumbnailStrip ThumbnailGenerator::generate(const QString& path, int frameCount, int thumbW, int thumbH) {
    ThumbnailStrip result;
    if (frameCount <= 0) return result;

    const QFileInfo info(path);
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = info.size();
    const QString key = memKey(path, frameCount, thumbW, thumbH);
    {
        QMutexLocker lock(&g_thumbCacheMutex);
        auto it = g_thumbCache.find(key);
        if (it != g_thumbCache.end() && it->modifiedMs == modifiedMs && it->sizeBytes == sizeBytes) {
            it->lastUsed = ++g_thumbUseCounter;
            return it->strip;
        }
    }
    if (VisualDiskCache::loadThumbnails(path, frameCount, thumbW, thumbH, &result)) {
        rememberStrip(key, result, modifiedMs, sizeBytes);
        return result;
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
    const int videoStreamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (videoStreamIndex < 0 || !decoder) {
        avformat_close_input(&fmtCtx);
        return result; // no video stream — nothing to visualize, not an error
    }

    AVStream* stream = fmtCtx->streams[videoStreamIndex];
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (static_cast<int>(i) != videoStreamIndex) fmtCtx->streams[i]->discard = AVDISCARD_ALL;
    }

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

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    if (!codecCtx
        || avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0
        || avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }
    // Deblocking only hides block edges, which a 120x68 thumbnail can't show.
    codecCtx->skip_loop_filter = AVDISCARD_ALL;

    SwsContext* sws = sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        thumbW, thumbH, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    std::vector<uint8_t> rgbBuffer(static_cast<size_t>(thumbW) * thumbH * 3);
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer.data(),
                          AV_PIX_FMT_RGB24, thumbW, thumbH, 1);

    AVPacket* packet = av_packet_alloc();

    // How far before its target a thumbnail may come from. Half the spacing
    // between thumbnails means neighbouring cells can never show the same
    // moment, and at filmstrip density nobody can tell a frame half a cell
    // early from the exact one.
    const double toleranceSec = std::max(0.5, 0.5 * durationSec / frameCount);

    // Safety cap on frames decoded for one thumbnail, for streams whose
    // timestamps never reach the target.
    constexpr int kMaxWalkFrames = 1000;

    // Where the decoder currently is (pts of the last frame it produced), and
    // whether it can be continued from — false after a seek, a failure, or a
    // capture made in keyframe-only mode (see below). Plus the longest
    // distance any thumbnail has had to decode forward from its keyframe: a
    // running estimate of the stream's keyframe spacing.
    double decoderAtSec = 0.0;
    bool decoderContinuable = false;
    double longestWalkSec = 0.0;

    auto seekTo = [&](double sec, AVDiscard mode) {
        const int64_t ts = static_cast<int64_t>(sec * AV_TIME_BASE);
        avformat_seek_file(fmtCtx, -1, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx);
        codecCtx->skip_frame = mode;
    };
    // Non-reference frames (B-frames, usually most of a stream) can be dropped
    // without decoding while still far from the target — nothing refers to
    // them. Near the target every frame is decoded so the pick is exact.
    auto walkMode = [](double gapSec) { return gapSec > 1.0 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT; };

    for (int i = 0; i < frameCount; ++i) {
        const double targetSec = durationSec * (static_cast<double>(i) / frameCount);

        // Three ways to reach the target, cheapest first:
        //
        //  1. Seek and decode ONLY the keyframe the seek lands on. If it's
        //     within tolerance, it's the thumbnail — one decode. This is the
        //     normal case for long files, where thumbnails sit minutes apart
        //     and keyframes seconds apart.
        //  2. If that keyframe is too far back, seek again and walk forward,
        //     skipping non-reference frames until close. The second seek is
        //     needed because packets fed during the keyframe-only probe were
        //     thrown away undecoded, and later frames depend on some of them —
        //     switching modes mid-stream would decode garbage until the next
        //     keyframe. Costs one extra keyframe decode.
        //  3. If the previous thumbnail left the decoder (validly) less than
        //     one keyframe-spacing behind this target, don't seek: carry on.
        //     A seek would land on the same keyframe and redo that walk.
        //
        // The old approach was always a full walk: seek, then decode every
        // frame from the keyframe — up to a whole GOP per thumbnail, on every
        // zoom-in.
        const bool carryOn = decoderContinuable && targetSec > decoderAtSec
                          && (targetSec - decoderAtSec) <= longestWalkSec;
        bool probing = !carryOn; // keyframe-only mode
        if (carryOn) codecCtx->skip_frame = walkMode(targetSec - decoderAtSec);
        else seekTo(targetSec, AVDISCARD_NONKEY);
        decoderContinuable = false;

        double walkStartSec = std::numeric_limits<double>::quiet_NaN();
        int attempts = 0;
        bool restartAsWalk = false;

        // Pulls every frame the decoder has ready; true once the target's
        // frame has been captured (anything after it stays buffered for the
        // next thumbnail's carry-on).
        auto takeReadyFrames = [&]() -> bool {
            while (avcodec_receive_frame(codecCtx, frame) == 0) {
                ++attempts;
                const int64_t rawPts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : frame->best_effort_timestamp;
                const double framePts = rawPts * av_q2d(stream->time_base);
                if (std::isnan(walkStartSec)) walkStartSec = framePts;
                decoderAtSec = framePts;

                if (framePts < targetSec - toleranceSec && attempts < kMaxWalkFrames) {
                    if (probing) { restartAsWalk = true; return false; } // keyframe too early: case 2
                    codecCtx->skip_frame = walkMode(targetSec - framePts);
                    continue;
                }

                sws_scale(sws, frame->data, frame->linesize, 0, codecCtx->height,
                          rgbFrame->data, rgbFrame->linesize);
                QImage img(rgbFrame->data[0], thumbW, thumbH, rgbFrame->linesize[0], QImage::Format_RGB888);
                result.frames.push_back(img.copy()); // own the memory — rgbBuffer gets reused next loop
                longestWalkSec = std::max(longestWalkSec, framePts - walkStartSec);
                // A keyframe-only capture leaves undecoded gaps behind it, so
                // nothing after it can be continued from.
                decoderContinuable = !probing;
                return true;
            }
            return false;
        };

        bool gotFrame = carryOn && takeReadyFrames();
        while (!gotFrame && av_read_frame(fmtCtx, packet) >= 0) {
            if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecCtx, packet) == 0) {
                gotFrame = takeReadyFrames();
            }
            av_packet_unref(packet);
            if (restartAsWalk) {
                restartAsWalk = false;
                probing = false;
                seekTo(targetSec, walkMode(targetSec - decoderAtSec));
                walkStartSec = std::numeric_limits<double>::quiet_NaN();
            }
        }

        if (!gotFrame) {
            // Couldn't land a frame near this timestamp (e.g. near EOF on an
            // unusual container) — push a blank placeholder rather than a
            // gap, which would desync the slice-mapping math at draw time.
            QImage blank(thumbW, thumbH, QImage::Format_RGB888);
            blank.fill(Qt::black);
            result.frames.push_back(blank);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&rgbFrame);
    av_frame_free(&frame);
    sws_freeContext(sws);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    result.durationSec = durationSec;
    if (!result.frames.isEmpty()) {
        rememberStrip(key, result, modifiedMs, sizeBytes);
        VisualDiskCache::saveThumbnails(path, frameCount, thumbW, thumbH, result);
    }
    return result;
}
