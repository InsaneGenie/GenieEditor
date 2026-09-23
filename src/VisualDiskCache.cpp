#include "VisualDiskCache.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr quint32 kWaveformMagic  = 0x47455756; // "GEWV"
constexpr quint32 kThumbnailMagic = 0x47455448; // "GETH"
constexpr quint32 kFormatVersion  = 1;

QString cacheDir() {
    static const QString dir = [] {
        const QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/visuals";
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

// The file identity goes INTO the name, so a changed source simply stops
// matching — there's never a stale entry to detect or clean up in place.
QString entryPath(const QString& mediaPath, const QString& kind, const QString& params) {
    const QFileInfo info(mediaPath);
    const QString identity = QStringLiteral("%1|%2|%3|%4|%5")
        .arg(info.absoluteFilePath())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.size())
        .arg(kind, params);
    const QByteArray hash = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex();
    return cacheDir() + "/" + QString::fromLatin1(hash) + "." + kind;
}

} // namespace

namespace VisualDiskCache {

bool loadWaveform(const QString& mediaPath, int peakCount, WaveformData* out) {
    QFile file(entryPath(mediaPath, "wave", QString::number(peakCount)));
    if (!file.open(QIODevice::ReadOnly)) return false;

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);
    in.setFloatingPointPrecision(QDataStream::SinglePrecision);

    quint32 magic = 0, version = 0;
    qint64 durationUs = 0;
    WaveformData data;
    in >> magic >> version >> durationUs >> data.peaks >> data.rms;
    if (in.status() != QDataStream::Ok || magic != kWaveformMagic || version != kFormatVersion
        || data.peaks.isEmpty() || data.peaks.size() != data.rms.size()) {
        return false;
    }
    // Duration as integer microseconds rather than a float: with the stream
    // in single precision (to keep the sample arrays half the size), a float
    // duration would be off by ~30ms at 80 hours.
    data.durationSec = durationUs / 1e6;
    *out = data;
    return true;
}

void saveWaveform(const QString& mediaPath, int peakCount, const WaveformData& data) {
    if (data.peaks.isEmpty()) return;
    QSaveFile file(entryPath(mediaPath, "wave", QString::number(peakCount)));
    if (!file.open(QIODevice::WriteOnly)) return;

    QDataStream outStream(&file);
    outStream.setVersion(QDataStream::Qt_6_0);
    outStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    outStream << kWaveformMagic << kFormatVersion
              << static_cast<qint64>(data.durationSec * 1e6) << data.peaks << data.rms;
    file.commit(); // atomic: a crash mid-write leaves no half-file behind
}

bool loadThumbnails(const QString& mediaPath, int frameCount, int thumbW, int thumbH, ThumbnailStrip* out) {
    QFile file(entryPath(mediaPath, "thumbs", QStringLiteral("%1x%2x%3").arg(frameCount).arg(thumbW).arg(thumbH)));
    if (!file.open(QIODevice::ReadOnly)) return false;

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0, version = 0;
    qint64 durationUs = 0;
    qint32 count = 0;
    in >> magic >> version >> durationUs >> count;
    if (in.status() != QDataStream::Ok || magic != kThumbnailMagic || version != kFormatVersion
        || count <= 0 || count > 100000) {
        return false;
    }

    ThumbnailStrip strip;
    strip.frames.reserve(count);
    for (qint32 i = 0; i < count; ++i) {
        QByteArray encoded;
        in >> encoded;
        QImage img;
        if (in.status() != QDataStream::Ok || !img.loadFromData(encoded, "JPG")) return false;
        strip.frames.push_back(img);
    }
    strip.durationSec = durationUs / 1e6;
    *out = strip;
    return true;
}

void saveThumbnails(const QString& mediaPath, int frameCount, int thumbW, int thumbH, const ThumbnailStrip& strip) {
    if (strip.frames.isEmpty()) return;
    QSaveFile file(entryPath(mediaPath, "thumbs", QStringLiteral("%1x%2x%3").arg(frameCount).arg(thumbW).arg(thumbH)));
    if (!file.open(QIODevice::WriteOnly)) return;

    QDataStream outStream(&file);
    outStream.setVersion(QDataStream::Qt_6_0);
    outStream << kThumbnailMagic << kFormatVersion
              << static_cast<qint64>(strip.durationSec * 1e6) << static_cast<qint32>(strip.frames.size());
    // JPEG at 120x68 is a few KB a frame — a full 200-frame strip is well under
    // a megabyte, where raw RGB would be ~5MB.
    for (const QImage& img : strip.frames) {
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        img.save(&buffer, "JPG", 82);
        outStream << encoded;
    }
    file.commit();
}

} // namespace VisualDiskCache
