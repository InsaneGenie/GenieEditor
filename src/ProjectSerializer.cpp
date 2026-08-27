#include "ProjectSerializer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>

namespace {

QString trackTypeToString(TrackType type) {
    switch (type) {
        case TrackType::Video:   return "video";
        case TrackType::Audio:   return "audio";
        case TrackType::Overlay: return "overlay";
    }
    return "video";
}

TrackType trackTypeFromString(const QString& s) {
    if (s == "audio") return TrackType::Audio;
    if (s == "overlay") return TrackType::Overlay;
    return TrackType::Video;
}

QJsonObject animatedPropertyToJson(const AnimatedProperty& prop) {
    QJsonObject obj;
    obj["static"] = prop.staticValue;
    if (!prop.keys.isEmpty()) {
        QJsonArray keys;
        for (const Keyframe& k : prop.keys) {
            QJsonObject key;
            key["t"] = k.timeSec;
            key["v"] = k.value;
            keys.append(key);
        }
        obj["keys"] = keys;
    }
    return obj;
}

void animatedPropertyFromJson(const QJsonObject& obj, AnimatedProperty& prop) {
    // A missing "static" keeps whatever the constructor set, which is how
    // OverlayAnimation's non-zero defaults (centred, 35% scale, opaque) survive
    // a file written before a property existed.
    if (obj.contains("static")) prop.staticValue = obj["static"].toDouble(prop.staticValue);

    prop.keys.clear();
    for (const QJsonValue& v : obj["keys"].toArray()) {
        const QJsonObject key = v.toObject();
        prop.keys.push_back(Keyframe{key["t"].toDouble(), key["v"].toDouble()});
    }
    // Sorted on read rather than trusted: the renderer's interpolation walks
    // this in order and would produce nonsense from a hand-edited file with
    // keys out of sequence.
    std::sort(prop.keys.begin(), prop.keys.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.timeSec < b.timeSec; });
}

QJsonObject transcriptSegmentToJson(const TranscriptSegment& seg) {
    QJsonObject obj;
    obj["text"] = seg.text;
    obj["start"] = seg.startSec;
    obj["end"] = seg.endSec;
    obj["source"] = seg.sourcePath;
    QJsonArray words;
    for (const TranscriptWord& w : seg.words) {
        QJsonObject word;
        word["text"] = w.text;
        word["start"] = w.startSec;
        word["end"] = w.endSec;
        words.append(word);
    }
    if (!words.isEmpty()) obj["words"] = words;
    return obj;
}

TranscriptSegment transcriptSegmentFromJson(const QJsonObject& obj) {
    TranscriptSegment seg;
    seg.text = obj["text"].toString();
    seg.startSec = obj["start"].toDouble();
    seg.endSec = obj["end"].toDouble();
    seg.sourcePath = obj["source"].toString();
    for (const QJsonValue& v : obj["words"].toArray()) {
        const QJsonObject word = v.toObject();
        seg.words.push_back(TranscriptWord{word["text"].toString(),
                                           word["start"].toDouble(),
                                           word["end"].toDouble()});
    }
    return seg;
}

// Resolves a clip's source, preferring the absolute path and falling back to
// the one relative to the project file. Returns an empty string when neither
// exists, which the caller reports rather than silently dropping the clip.
QString resolveSourcePath(const QJsonObject& clipObj, const QDir& projectDir) {
    const QString absolute = clipObj["source"].toString();
    if (!absolute.isEmpty() && QFileInfo::exists(absolute)) return absolute;

    const QString relative = clipObj["sourceRelative"].toString();
    if (!relative.isEmpty()) {
        const QString resolved = QDir::cleanPath(projectDir.absoluteFilePath(relative));
        if (QFileInfo::exists(resolved)) return resolved;
    }

    // Neither resolved. The absolute path is still returned when there was one:
    // keeping the clip on the timeline pointing at a path that doesn't exist is
    // far better than deleting it, because the edit is intact and re-linking is
    // a matter of putting the file back.
    return absolute;
}

} // namespace

bool ProjectSerializer::save(const Project& project, const QString& path,
                             double playheadSec, double pixelsPerSecond, QString* error) {
    const QDir projectDir = QFileInfo(path).absoluteDir();

    QJsonObject root;
    root["format"] = formatId();
    root["version"] = kFormatVersion;
    root["playheadSec"] = playheadSec;
    root["pixelsPerSecond"] = pixelsPerSecond;

    QJsonArray tracksJson;
    for (const Track& track : project.tracks) {
        QJsonObject trackObj;
        trackObj["type"] = trackTypeToString(track.type);
        trackObj["name"] = track.name;
        trackObj["muted"] = track.muted;
        trackObj["enabled"] = track.enabled;
        trackObj["gainDb"] = track.gainDb;
        trackObj["volumePercent"] = track.volumePercent;
        trackObj["heightPx"] = track.heightPx;
        trackObj["colorIndex"] = track.colorIndex;
        trackObj["pairedAudioTrackIndex"] = track.pairedAudioTrackIndex;

        if (!track.transcript.isEmpty()) {
            QJsonArray segments;
            for (const TranscriptSegment& seg : track.transcript) {
                segments.append(transcriptSegmentToJson(seg));
            }
            trackObj["transcript"] = segments;
            trackObj["transcriptSourcePath"] = track.transcriptSourcePath;
            trackObj["transcriptSignature"] = track.transcriptSignature;
        }

        QJsonArray clipsJson;
        for (const Clip& clip : track.clips) {
            QJsonObject clipObj;
            clipObj["source"] = clip.sourcePath;
            clipObj["sourceRelative"] = projectDir.relativeFilePath(clip.sourcePath);
            clipObj["in"] = clip.sourceInSec;
            clipObj["out"] = clip.sourceOutSec;
            clipObj["pos"] = clip.trackPosSec;
            clipObj["gainDb"] = clip.gainDb;
            // Only written when it isn't the default, so an ordinary project
            // file stays free of a "speed": 1 on every clip.
            if (clip.speed != 1.0) clipObj["speed"] = clip.speed;

            // Only overlay clips use the animation block, and writing five
            // default-valued property objects for every video clip in a long
            // project is noise in a file meant to stay readable.
            if (track.type == TrackType::Overlay) {
                QJsonObject anim;
                anim["x"] = animatedPropertyToJson(clip.anim.x);
                anim["y"] = animatedPropertyToJson(clip.anim.y);
                anim["scale"] = animatedPropertyToJson(clip.anim.scale);
                anim["opacity"] = animatedPropertyToJson(clip.anim.opacity);
                anim["rotation"] = animatedPropertyToJson(clip.anim.rotation);
                clipObj["anim"] = anim;
            }
            clipsJson.append(clipObj);
        }
        trackObj["clips"] = clipsJson;
        tracksJson.append(trackObj);
    }
    root["tracks"] = tracksJson;

    QJsonArray markersJson;
    for (const Marker& marker : project.markers) {
        QJsonObject markerObj;
        markerObj["start"] = marker.startSec;
        markerObj["end"] = marker.endSec;
        markerObj["label"] = marker.label;
        markerObj["color"] = marker.color.name(QColor::HexArgb);
        markersJson.append(markerObj);
    }
    root["markers"] = markersJson;

    // QSaveFile writes to a temporary and renames on commit, so an interrupted
    // save leaves the previous version intact instead of a truncated file where
    // the project used to be.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QString("Couldn't open %1 for writing: %2")
                                .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = QString("Couldn't finish writing %1: %2")
                                .arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    return true;
}

ProjectSerializer::LoadResult ProjectSerializer::load(Project& project, const QString& path) {
    LoadResult result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.error = QString("Couldn't open %1: %2")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QString("%1 isn't a readable project file (%2).")
                           .arg(QFileInfo(path).fileName(), parseError.errorString());
        return result;
    }

    const QJsonObject root = doc.object();
    const QString format = root["format"].toString();
    if (format != formatId() && format != legacyFormatId()) {
        result.error = QString("%1 isn't a GenieEditor project file.")
                           .arg(QFileInfo(path).fileName());
        return result;
    }
    if (root["version"].toInt() > kFormatVersion) {
        result.error = QString("%1 was saved by a newer version of this app "
                               "(file format %2, this build understands %3).")
                           .arg(QFileInfo(path).fileName())
                           .arg(root["version"].toInt())
                           .arg(kFormatVersion);
        return result;
    }

    // Built into a SEPARATE project and only swapped in at the end, so a file
    // that turns out to be malformed halfway through can't leave the caller
    // holding a half-loaded timeline.
    Project loaded;
    const QDir projectDir = QFileInfo(path).absoluteDir();

    for (const QJsonValue& tv : root["tracks"].toArray()) {
        const QJsonObject trackObj = tv.toObject();
        Track track;
        track.type = trackTypeFromString(trackObj["type"].toString());
        track.name = trackObj["name"].toString();
        track.muted = trackObj["muted"].toBool(false);
        track.enabled = trackObj["enabled"].toBool(true);
        track.gainDb = trackObj["gainDb"].toDouble(0.0);
        track.volumePercent = trackObj["volumePercent"].toInt(100);
        track.heightPx = trackObj["heightPx"].toInt(0);
        track.colorIndex = trackObj["colorIndex"].toInt(0);
        track.pairedAudioTrackIndex = trackObj["pairedAudioTrackIndex"].toInt(-1);
        track.transcriptSourcePath = trackObj["transcriptSourcePath"].toString();
        track.transcriptSignature = trackObj["transcriptSignature"].toString();

        for (const QJsonValue& sv : trackObj["transcript"].toArray()) {
            track.transcript.push_back(transcriptSegmentFromJson(sv.toObject()));
        }

        for (const QJsonValue& cv : trackObj["clips"].toArray()) {
            const QJsonObject clipObj = cv.toObject();
            Clip clip;
            clip.sourcePath = resolveSourcePath(clipObj, projectDir);
            clip.sourceInSec = clipObj["in"].toDouble();
            clip.sourceOutSec = clipObj["out"].toDouble();
            clip.trackPosSec = clipObj["pos"].toDouble();
            clip.gainDb = clipObj["gainDb"].toDouble(0.0);
            // Absent means 1.0, which is what makes files written before speed
            // existed load as ordinary full-rate clips.
            clip.speed = clipObj["speed"].toDouble(1.0);

            if (clipObj.contains("anim")) {
                const QJsonObject anim = clipObj["anim"].toObject();
                animatedPropertyFromJson(anim["x"].toObject(), clip.anim.x);
                animatedPropertyFromJson(anim["y"].toObject(), clip.anim.y);
                animatedPropertyFromJson(anim["scale"].toObject(), clip.anim.scale);
                animatedPropertyFromJson(anim["opacity"].toObject(), clip.anim.opacity);
                animatedPropertyFromJson(anim["rotation"].toObject(), clip.anim.rotation);
            }

            if (!clip.sourcePath.isEmpty() && !QFileInfo::exists(clip.sourcePath)
                && !result.missingMedia.contains(clip.sourcePath)) {
                result.missingMedia.push_back(clip.sourcePath);
            }

            // A zero-length clip is invisible and unselectable — it could never
            // be clicked to fix, so it's dropped rather than left as a mystery.
            if (clip.durationSec() > 0.0) track.clips.push_back(clip);
        }
        loaded.tracks.push_back(track);
    }

    for (const QJsonValue& mv : root["markers"].toArray()) {
        const QJsonObject markerObj = mv.toObject();
        Marker marker;
        marker.startSec = markerObj["start"].toDouble();
        marker.endSec = markerObj["end"].toDouble();
        marker.label = markerObj["label"].toString();
        const QColor colour(markerObj["color"].toString());
        if (colour.isValid()) marker.color = colour;
        loaded.markers.push_back(marker);
    }

    // A project with no tracks would leave the app in a state it can't reach
    // through its own UI — every import path assumes track 0 exists.
    if (loaded.tracks.isEmpty()) {
        result.error = QString("%1 contains no tracks.").arg(QFileInfo(path).fileName());
        return result;
    }

    project = loaded;
    result.ok = true;
    result.playheadSec = root["playheadSec"].toDouble(0.0);
    result.pixelsPerSecond = root["pixelsPerSecond"].toDouble(0.0);
    return result;
}

bool ProjectSerializer::hasProjectExtension(const QString& path) {
    const QString suffix = QFileInfo(path).suffix();
    return suffix.compare(fileExtension(), Qt::CaseInsensitive) == 0
        || suffix.compare(legacyFileExtension(), Qt::CaseInsensitive) == 0;
}
