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

    const double splitOffset = timelinePosSec - original.trackPosSec;

    Clip second = original;
    second.sourceInSec = original.sourceInSec + splitOffset;
    second.trackPosSec = timelinePosSec;

    original.sourceOutSec = original.sourceInSec + splitOffset;

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
