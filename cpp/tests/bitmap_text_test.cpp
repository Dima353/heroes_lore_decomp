#include "bitmap_font.hpp"
#include "language.hpp"
#include "language_pack.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main() {
    try {
        BitmapFont font; font.height=8;font.space_width=2;
        for(int i=0;i<95;++i) font.glyph_offsets.push_back(i*4);
        const auto words=layout_bitmap_text(font,"aaa bb",0,0,20);
        check(words.size()==5 && words[3].x==0 && words[3].y==8 && words[4].y==8,"wrap at word boundary");
        check(words[2].text_end==3 && words[3].text_end==5,"reveal offsets retain spaces across wrapped lines");
        const auto utf8=layout_bitmap_text(font,"A\xc3\xa9 B",0,0,100);
        check(utf8[1].text_end==3 && utf8[2].text_end==5,"reveal offsets preserve UTF-8 boundaries");
        const auto long_word=layout_bitmap_text(font,"aaaaaaa",0,0,20);
        check(long_word.size()==7 && long_word[5].y==8,"long words do not overflow");
        check(format_game_text("one;two")=="one\ntwo","resource semicolons become line breaks");
        // Russian bank: 'n' is the font's own й glyph, digits and spaces stay ASCII.
        check(decode_russian_font_text("Ln 1")=="\xd0\x9c\xd0\xb9 1","Russian bank letters decode to Cyrillic");
        BitmapFont cyrillic=font;cyrillic.atlas.width=95*4;cyrillic.atlas.height=8;
        cyrillic.atlas.rgba.assign(std::size_t(95*4*8*4),0);
        for(int y=2;y<8;++y) cyrillic.atlas.rgba[std::size_t((y*95*4+('F'-33)*4)*4+3)]=255;
        cyrillic.extra_glyphs.push_back({0x415,std::uint32_t(('F'-33)*4),4});
        add_marked_glyph(cyrillic,0x415,0x401,true);
        check(cyrillic.extra_glyphs.back().codepoint==0x401 && cyrillic.atlas.width==95*4+4 &&
              cyrillic.atlas.rgba[std::size_t((0*cyrillic.atlas.width+95*4)*4+3)]==255,"Yo is E with a diaeresis");
        std::cout << "Bitmap text checks passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
