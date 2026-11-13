#pragma once
// Fonts and the text bank read from the JAR (with the Russian pack), dialogue paging and the game JAR.
#include <filesystem>
#include <string>
#include <vector>
#include "bitmap_font.hpp"
#include "map.hpp"
#include "platform.hpp"
#include "sprite.hpp"
#include "viewport.hpp"

DialogueLayout shown_dialogue_layout(int output_w, int output_h, int font_height, int portrait);
int dialogue_line_count(const BitmapFont& font, const std::string& text,
                        int output_w, int output_h, int portrait = 0);
void prepare_dialogue_font(BitmapFont& font);
BitmapFont load_text_font(GameArchive& archive,const std::string& path,void (*prepare)(BitmapFont&));
std::vector<std::string> load_text_bank(const GameArchive& archive);
BitmapFont load_heading_font(GameArchive& archive,DecodedImage& black_text);
std::string find_game_jar(const std::filesystem::path& folder);
#if !HEROES_LORE_CONSOLE
std::filesystem::path desktop_program_dir();
std::string find_desktop_jar();
#endif
