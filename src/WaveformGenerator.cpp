#include "WaveformGenerator.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <vector>
#include <cmath>
#include <algorithm>

WaveformData WaveformGenerator::generate(const QString& path, int peakCount) {
    WaveformData result;
    if (peakCount <= 0) return result;

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
    double bucketMax = 0.0;
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
                    bucketMax = std::max(bucketMax, static_cast<double>(std::fabs(monoBuf[i])));
                    sampleCounter += 1.0;
                    if (sampleCounter >= samplesPerBucket) {
                        result.peaks.push_back(static_cast<float>(bucketMax));
                        bucketMax = 0.0;
                        sampleCounter = 0.0;
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    if (sampleCounter > 0.0) {
        result.peaks.push_back(static_cast<float>(bucketMax));
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    result.durationSec = durationSec;
    return result;
}
