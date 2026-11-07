#pragma once

#include "map.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

std::vector<std::string> parse_language_bank(const std::vector<std::uint8_t>& bytes);
std::vector<std::string> load_language_bank(const GameArchive& archive,
                                            const std::string& path);
std::optional<std::string> resolve_event_text(
    const EventGrid& event, std::size_t event_string_index,
    const std::vector<std::string>& language_bank);
// The language-bank index an event string refers to (the port records recipe lines by it).
std::optional<std::size_t> event_text_bank_index(const EventGrid& event, std::size_t event_string_index);


// bh.a(int/String) converts the resource's semicolon line separators.
std::string format_game_text(std::string text);
std::string format_dialogue_text(std::string text);

// z: u8 count, then u16 byte length + ASCII decimal language key per entry.
std::vector<std::string> parse_tdf_text(const std::vector<std::uint8_t>& bytes,
    const std::vector<std::string>& language_bank);
