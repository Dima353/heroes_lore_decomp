#pragma once
#include <cstdlib>
#include <string>
#include <vector>

// as state 12 (as.d): /sgui/ed<hero> story pages, then state 13 (as.e): /sgui/edsr staff roll.
// One tick is one as.paint frame. Positions are in the original screen's pixels.
struct EndingSequence {
    enum class Phase { story, credits, done };
    struct CreditLine { std::string text; int y{}; };
    Phase phase{Phase::story};
    std::vector<std::string> story, staff;
    int counter{-16};       // as.e: white fade (-16..0), then the page / roll timer
    int first{}, end{-1};   // as.t / as.u: current story page is lines [first, end)
    bool advance{};         // as.i: any key turns the page
    std::vector<CreditLine> rolling;
    int next_staff{};       // as.t during the roll

    void start(std::vector<std::string> story_lines, std::vector<std::string> staff_lines) {
        *this = EndingSequence{};
        story = std::move(story_lines);
        staff = std::move(staff_lines);
    }
    void press() { if (phase == Phase::story) advance = true; }
    // White level of the opening fade (255 -> 0), or -1 once the story is shown.
    int fade_level() const { return phase == Phase::story && counter < 0 ? 255 * -counter / 16 : -1; }

    void tick(int screen_height) {
        if (phase == Phase::story) tick_story();
        else if (phase == Phase::credits) tick_credits(screen_height);
    }

private:
    void tick_story() {
        if (counter < 0) { ++counter; return; }
        if (advance || end == -1) {
            advance = false;
            first = ++end;
            // Lines up to a "_N" marker form a page; N restarts the page timer.
            for (; end < int(story.size()); ++end)
                if (!story[std::size_t(end)].empty() && story[std::size_t(end)][0] == '_') {
                    counter = std::atoi(story[std::size_t(end)].c_str() + 1);
                    break;
                }
        }
        if (end >= int(story.size())) { // as.h(): the staff roll follows
            phase = Phase::credits;
            counter = 0;
            return;
        }
        if (counter > 0) --counter;
    }
    void tick_credits(int screen_height) {
        if (counter == 0 && next_staff < int(staff.size())) {
            const auto& line = staff[std::size_t(next_staff++)];
            if (!line.empty() && line[0] == '-') counter = 4;
            else if (!line.empty() && line[0] == '=') counter = 10;
            else { rolling.push_back({line, screen_height}); counter = 5; }
        }
        if (next_staff >= int(staff.size()) && rolling.empty()) { phase = Phase::done; return; }
        if (counter > 0) --counter;
        for (std::size_t i = rolling.size(); i-- > 0;) {
            rolling[i].y -= 2;
            if (rolling[i].y < -8) rolling.erase(rolling.begin() + std::ptrdiff_t(i));
        }
    }
};
