#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>

// Byte offsets keep UTF-8 intact while counting original character steps.
struct DialogueReveal {
    std::size_t end{};
    void reset() {end=0;}
    void finish() {end=std::numeric_limits<std::size_t>::max();}
    bool complete(std::size_t page_end) const {return end>=page_end;}
    void tick(const std::string& text,std::size_t page_start,std::size_t page_end,bool instant) {
        end=std::max(end,page_start);
        if(instant) {end=page_end;return;}
        for(int count=0;count<2 && end<page_end && end<text.size();++count) {
            ++end;
            while(end<text.size() && (static_cast<unsigned char>(text[end])&0xc0)==0x80) ++end;
        }
    }
};
