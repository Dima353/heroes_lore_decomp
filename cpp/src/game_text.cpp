#include <SDL.h>
#include "game_text.hpp"
#include "language.hpp"
#include "language_pack.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>

// The window dialogue pages are counted in: 3DS shows it on the bottom screen.
DialogueLayout shown_dialogue_layout(int output_w, int output_h, int font_height, int portrait) {
#if HEROES_LORE_3DS
    (void)output_w;(void)output_h;
    return calculate_bottom_dialogue_layout(font_height,portrait);
#else
    return calculate_dialogue_layout(output_w,output_h,font_height,portrait);
#endif
}

int dialogue_line_count(const BitmapFont& font, const std::string& text,
                               int output_w, int output_h, int portrait) {
    const auto layout = shown_dialogue_layout(output_w,output_h,font.height,portrait);
    const int line_height = static_cast<int>(font.height)*layout.scale;
    const auto glyphs = layout_bitmap_text(font,text,0,0,layout.inner.w,layout.scale);
    int last_line = 0;
    for (const auto& glyph : glyphs) last_line = std::max(last_line,glyph.y/line_height);
    return last_line+1;
}
void prepare_dialogue_font(BitmapFont& font) {recolor_bitmap_font(font,0xffffff,0x000000);}
// Russian pack: the JAR's English font plus the Russian glyphs under their real Cyrillic
// codepoints, so Latin port strings and Russian text share one font.
BitmapFont load_text_font(GameArchive& archive,const std::string& path,void (*prepare)(BitmapFont&)) {
    const auto* pack=archive.has_language_pack()?&russian_language_pack():nullptr;
    archive.set_language_pack(nullptr);
    BitmapFont font=load_bitmap_font(archive,path);
    archive.set_language_pack(pack);
    prepare(font);
    if(pack) {
        BitmapFont russian=load_bitmap_font(archive,path);
        prepare(russian);
        add_bitmap_glyphs(font,russian,russian_font_glyphs());
        // The Russian fonts have no Й, Ё or ё (they reuse И and Е/е): draw the marks in.
        add_marked_glyph(font,0x418,0x419,false);
        add_marked_glyph(font,0x415,0x401,true);
        add_marked_glyph(font,0x435,0x451,true);
    }
    return font;
}
// The Russian bank is written in the font's ASCII encoding; decode it to real Cyrillic.
std::vector<std::string> load_text_bank(const GameArchive& archive) {
    auto bank=load_language_bank(archive,"lang/language.fr-FR");
    if(archive.has_language_pack()) for(auto& text:bank) text=decode_russian_font_text(text);
    // The Russian translation left Rage's description (grdsk 3 -> 727) as a bare "6"; the English
    // one reads "Attacking force becomes stronger for a moment."
    if(bank.size()>727 && bank[727]=="6") {
        const std::string rage="Сила атаки ненадолго возрастает.";
        bank[727]=archive.has_language_pack()?rage:encode_russian_font_text(rage);
    }
    // gm 32 (1193) marks the active guardian; English "Equip" became the verb "Надеть" there.
    if(bank.size()>1193 && (bank[1193]=="Надеть" || bank[1193]==encode_russian_font_text("Надеть")))
        bank[1193]=archive.has_language_pack()?std::string("Надет"):encode_russian_font_text("Надет");
    // Russian lines that misdirect the player (wrong item, place or action), checked against the
    // English bank and the events (localization audit, 04.11). Applied only over the exact original.
    struct TextFix {std::size_t id; const char* original; const char* fixed;};
    static const TextFix fixes[]={
        // itm/mixtbl: will stone + water crystal + ruby -> (17,7) Orb of faint (392), not weakening (388).
        {1881,"[Рецепт] Сфера Слабости = Камень Воли + Кристалл Воды + Рубин",
              "[Рецепт] Сфера обморока = камень воли + кристалл воды + рубин"},
        {1856,"[Рецепт] Камень Жизни = корень + платина","[Рецепт] Камень здоровья = корень + платина"},
        {1305,"Николас сказал мне поймать 5 волков и принести 5 зубов. Если я его исполню его просьбу – он обучит меня новым навыкам",
              "Николас велел поймать пять волков и принести пять драконьих зубов. Если пройду испытание, он научит меня новому приёму."},
        {1410,"Крестоносцы томятся в подземной тюрьме Лакуса. С помощью Ланака освободите их",
              "Крестоносцы заперты в подземной тюрьме Солума. Освободим их вместе с Ланаком."},
        {1417,"Нужно узнать больше о Хранителях.Может мама Лесли что-нибудь знает",
              "Нужно больше узнать о Хранителях. Может, мама или Лесли что-нибудь знают."},
        {3139,"|[Рея] Я должна идти первой...","|[Рея] К кому пойти сначала?.."},
        {1473,"Найджел сказал,что видел пещеру в Эребос в Центральных горах",
              "Найджел сказал, что из пещеры в Центральных горах можно увидеть Эребос."},
        {3696,"$[Нед] Давай быстрее! Шевелись!","$[Нед] Уже вернулся? Отлично!"},
        // Lever (21,3): m/6/54.evt and m/8/54.evt use it; the messages report what was just done.
        {465,"|Предмет квеста;неработающий рычаг,который доставляет в нужное место.",
             "|Предмет задания;Пригодный к использованию рычаг, который можно вставить в подходящее гнездо."},
        {3486,"$[Арамор] Во всех колоннах не хватает кирпичей. Я могу вставить в них что-нибудь...",
              "$[Арамор] В каждой колонне не хватает одного кирпича. Можно что-нибудь вставить в эти места..."},
        {3497,"Выключите переключатель на левой колонне используя рычаг.","Переключатель на левой колонне выключен с помощью рычага."},
        {3498,"Включите переключатель на левой колонне используя рычаг.","Переключатель на левой колонне включён с помощью рычага."},
        {3499,"Выключите переключатель на средней колонне используя рычаг.","Переключатель на средней колонне выключен с помощью рычага."},
        {3500,"Включите переключатель на средней колонне используя рычаг.","Переключатель на средней колонне включён с помощью рычага."},
        {3501,"Выключите переключатель на правой колонне используя рычаг.","Переключатель на правой колонне выключен с помощью рычага."},
        {3502,"Включите переключатель на правой колонне используя рычаг.","Переключатель на правой колонне включён с помощью рычага."},
        {3503,"Вставьте рычаг в колонну.","Рычаг вставлен в колонну."},
        {3504,"Я не могу. Попробуйте использовать рычаг снова.","Не могу спуститься. Нужно снова воспользоваться рычагом."},
    };
    if(archive.has_language_pack())
        for(const auto& fix:fixes) if(fix.id<bank.size() && bank[fix.id]==fix.original) bank[fix.id]=fix.fixed;
    return bank;
}
// bh.d/bh.e: fonts/big.mf as it is, fill plus outline. The Russian release writes its headings
// (3920.., help topics) in the big font's own layout (MOBAR ULPA = НОВАЯ ИГРА), so that font is
// not merged with the Latin one. The fill is the rarer palette colour (the Russian font swaps them).
// font.atlas gets white fill on black outline (bh.e), black_text the inverse (bh.d).
BitmapFont load_heading_font(GameArchive& archive,DecodedImage& black_text) {
    BitmapFont font=load_bitmap_font(archive,"fonts/big.mf");
    auto& rgba=font.atlas.rgba;
    const auto rgb_at=[&](std::size_t p){return (std::uint32_t(rgba[p])<<16)|(std::uint32_t(rgba[p+1])<<8)|rgba[p+2];};
    if(font.has_shadow_palette) {
        std::size_t fill=0,outline=0;
        for(std::size_t p=0;p+3<rgba.size();p+=4) {
            if(!rgba[p+3]) continue;
            if(rgb_at(p)==font.text_palette_rgb) ++fill; else if(rgb_at(p)==font.shadow_palette_rgb) ++outline;
        }
        if(fill>outline) std::swap(font.text_palette_rgb,font.shadow_palette_rgb);
    }
    recolor_bitmap_font(font,0xffffff,0x000000);
    black_text=font.atlas;
    for(std::size_t p=0;p+3<black_text.rgba.size();p+=4)
        if(black_text.rgba[p+3]) for(int k=0;k<3;++k) black_text.rgba[p+k]=std::uint8_t(255-black_text.rgba[p+k]);
    return font;
}
// The game JAR in a folder: an English one first (the port's Russian comes from Options),
// otherwise the first .jar by name; empty when there is none.
std::string find_game_jar(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> jars;
    std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator(folder,error))
        if(entry.is_regular_file(error) && entry.path().extension()==".jar") jars.push_back(entry.path());
    if(jars.empty()) return {};
    std::sort(jars.begin(),jars.end());
    for(const auto& jar:jars) if(jar.filename().string().find("ENG")!=std::string::npos) return jar.string();
    return jars.front().string();
}
#if !HEROES_LORE_CONSOLE
// Desktop: the folder of the program; for an AppImage the folder of the .AppImage file
// (the program itself runs from a read-only mount).
std::filesystem::path desktop_program_dir() {
    if(const char* appimage=std::getenv("APPIMAGE"); appimage && *appimage)
        return std::filesystem::u8path(appimage).parent_path();
    std::filesystem::path base=".";
    if(char* path=SDL_GetBasePath()) {base=std::filesystem::u8path(path);SDL_free(path);}
    return base;
}
// Desktop: the JAR beside the program.
std::string find_desktop_jar() {
    return find_game_jar(desktop_program_dir());
}
#endif
