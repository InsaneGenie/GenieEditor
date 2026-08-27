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
