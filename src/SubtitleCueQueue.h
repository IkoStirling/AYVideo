#pragma once
// SubtitleCueQueue.h — decode-thread producer / player-thread consumer
// soft-cue mailbox (text subtitle packets → SubtitleCue).

#include <AYVideo/VideoSubtitle.h>

#include <mutex>
#include <vector>

namespace ayt::video
{

class SubtitleCueQueue
{
public:
    void push(SubtitleCue cue)
    {
        std::lock_guard<std::mutex> lock(_mu);
        _cues.push_back(std::move(cue));
    }

    void drainTo(std::vector<SubtitleCue>& out)
    {
        std::lock_guard<std::mutex> lock(_mu);
        if (_cues.empty())
        {
            return;
        }
        out.insert(out.end(),
                   std::make_move_iterator(_cues.begin()),
                   std::make_move_iterator(_cues.end()));
        _cues.clear();
    }

    void clear() noexcept
    {
        std::lock_guard<std::mutex> lock(_mu);
        _cues.clear();
    }

private:
    std::mutex _mu;
    std::vector<SubtitleCue> _cues;
};

} // namespace ayt::video
