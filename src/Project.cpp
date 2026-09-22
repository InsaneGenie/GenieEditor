#include "Project.h"
#include <algorithm>

bool Project::splitClipAt(int trackIndex, int clipIndex, double timelinePosSec) {
    if (trackIndex < 0 || trackIndex >= tracks.size())
        return false;

    Track& track = tracks[trackIndex];
    if (clipIndex < 0 || clipIndex >= track.clips.size())
        return false;

    Clip& original = track.clips[clipIndex];
    const double clipEnd = original.trackPosSec + original.durationSec();

    // Position must fall strictly inside the clip to produce a valid split.
    if (timelinePosSec <= original.trackPosSec || timelinePosSec >= clipEnd)
        return false;

    // The cut lands at a TIMELINE position, but both halves are described by
    // SOURCE in/out points. On a sped-up clip those differ by the speed factor,
    // so converting through the clip is the difference between splitting where
    // the playhead is and splitting somewhere else entirely.
    const double sourceSplitPoint = original.sourceTimeAt(timelinePosSec);

    Clip second = original;              // inherits speed, as it must: both
    second.sourceInSec = sourceSplitPoint; // halves are the same footage at the
    second.trackPosSec = timelinePosSec;   // same rate, just cut in two

    original.sourceOutSec = sourceSplitPoint;

    track.clips.insert(track.clips.begin() + clipIndex + 1, second);
    return true;
}

void Project::rippleDeleteRange(double startSec, double endSec) {
    const double gap = endSec - startSec;
    if (gap <= 0.0) return;

    // Positions come out of floating-point division, so a clip that ends
    // "exactly" at the cut can land a hair either side of it. Without a
    // tolerance that produces zero-length slivers that are invisible on the
    // timeline but still get handed to mpv and ffmpeg.
    constexpr double kEps = 1e-4;
    constexpr double kMinPieceSec = 0.01;

    for (Track& track : tracks) {
        QVector<Clip> kept;
        kept.reserve(track.clips.size() + 1);

        for (const Clip& clip : track.clips) {
            const double clipStart = clip.trackPosSec;
            const double clipEnd = clipStart + clip.durationSec();

            if (clipEnd <= startSec + kEps) { // wholly before the cut
                kept.push_back(clip);
                continue;
            }
            if (clipStart >= endSec - kEps) { // wholly after: slide left
                Clip moved = clip;
                moved.trackPosSec -= gap;
                kept.push_back(moved);
                continue;
            }

            // Overlaps the cut. Each surviving side is converted through the
            // clip's own time mapping, so a sped-up clip is trimmed at the
            // right SOURCE moment rather than the timeline difference.
            if (clipStart < startSec - kEps && startSec - clipStart >= kMinPieceSec) {
                Clip left = clip;
                left.sourceOutSec = clip.sourceTimeAt(startSec);
                kept.push_back(left);
            }
            if (clipEnd > endSec + kEps && clipEnd - endSec >= kMinPieceSec) {
                Clip right = clip;
                right.sourceInSec = clip.sourceTimeAt(endSec);
                right.trackPosSec = startSec;

                // Overlay keyframes are relative to the clip's own start, which
                // just moved later into the footage by this much. Shifting them
                // keeps each key on the moment it was set against, instead of
                // the whole animation sliding with the new start.
                const double localShift = endSec - clipStart;
                for (AnimatedProperty* prop : {&right.anim.x, &right.anim.y, &right.anim.scale,
                                               &right.anim.opacity, &right.anim.rotation}) {
                    for (Keyframe& k : prop->keys) k.timeSec -= localShift;
                }
                kept.push_back(right);
            }
        }
        track.clips = kept;
    }

    // A point inside the removed range collapses onto the cut. Pins that were
    // inside are dropped rather than stacked on the seam, since the moment
    // they marked no longer exists.
    auto mapTime = [&](double t) {
        if (t <= startSec) return t;
        if (t >= endSec) return t - gap;
        return startSec;
    };
    QVector<Marker> keptMarkers;
    for (const Marker& m : markers) {
        const bool wasPin = m.isPin();
        const bool startInside = m.startSec > startSec + kEps && m.startSec < endSec - kEps;
        if (wasPin && startInside) continue;

        Marker mapped = m;
        mapped.startSec = mapTime(m.startSec);
        mapped.endSec = mapTime(m.endSec);
        if (!wasPin && mapped.isPin()) continue; // a region wholly inside the cut
        keptMarkers.push_back(mapped);
    }
    markers = keptMarkers;
}

double Project::durationSec() const {
    double maxEnd = 0.0;
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            maxEnd = std::max(maxEnd, clip.trackPosSec + clip.durationSec());
        }
    }
    return maxEnd;
}

void Project::removeTrack(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= tracks.size()) return;

    tracks.removeAt(trackIndex);

    for (auto& t : tracks) {
        if (t.pairedAudioTrackIndex == trackIndex) {
            t.pairedAudioTrackIndex = -1; // its pair was the one just removed
        } else if (t.pairedAudioTrackIndex > trackIndex) {
            t.pairedAudioTrackIndex -= 1; // shift down to match the removal
        }
    }
}

QVector<TranscriptRow> buildTranscriptRows(const Track& track) {
    QVector<TranscriptRow> rows;

    // Driven by the CLIPS, not by the segment list. Walking segments and asking
    // "where does this land" cannot answer correctly when a file is on the
    // track twice — it has to pick one clip, and it always picked the first.
    // Walking clips and asking "what do you play" has an exact answer for each.
    for (int c = 0; c < track.clips.size(); ++c) {
        const Clip& clip = track.clips[c];

        for (int seg = 0; seg < track.transcript.size(); ++seg) {
            const TranscriptSegment& segment = track.transcript[seg];
            if (segment.sourcePath != clip.sourcePath) continue;

            // Overlap, not containment: a line beginning just before the
            // in-point is still partly audible, and dropping it would lose real
            // dialogue. Lines wholly outside the trim are skipped — they exist
            // in the file but not in the edit, and giving them a ruler position
            // would be inventing one.
            if (segment.endSec <= clip.sourceInSec) continue;
            if (segment.startSec >= clip.sourceOutSec) continue;

            // Clamped, so a line running in from before the cut is stamped at
            // the moment it becomes audible rather than before the clip starts.
            const double audibleSourceSec = std::max(segment.startSec, clip.sourceInSec);

            // Through the clip, so speed is accounted for: a line four source
            // seconds into a 4x clip is heard one second in.
            rows.push_back({clip.timelineTimeAt(audibleSourceSec), seg, c});
        }
    }

    std::sort(rows.begin(), rows.end(), [](const TranscriptRow& a, const TranscriptRow& b) {
        return a.timelineSec < b.timelineSec;
    });
    return rows;
}
