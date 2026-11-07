#include "language.hpp"

#include <charconv>
#include <utility>
#include <stdexcept>

namespace {
std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("truncated language-bank u32");
    return (std::uint32_t(bytes[offset]) << 24) |
           (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) |
           std::uint32_t(bytes[offset + 3]);
}

std::uint16_t read_u16_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw std::runtime_error("truncated language-bank u16");
    return static_cast<std::uint16_t>((std::uint16_t(bytes[offset]) << 8) |
                                      bytes[offset + 1]);
}

void append_utf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string decode_modified_utf8(const std::uint8_t* data, std::size_t length) {
    std::vector<std::uint16_t> units;
    units.reserve(length);
    for (std::size_t i = 0; i < length;) {
        const auto first = data[i++];
        if ((first & 0x80) == 0) {
            units.push_back(first);
        } else if ((first & 0xe0) == 0xc0) {
            if (i >= length || (data[i] & 0xc0) != 0x80)
                throw std::runtime_error("invalid modified UTF-8 two-byte sequence");
            const auto second = data[i++];
            units.push_back(static_cast<std::uint16_t>(((first & 0x1f) << 6) |
                                                       (second & 0x3f)));
        } else if ((first & 0xf0) == 0xe0) {
            if (length - i < 2 || (data[i] & 0xc0) != 0x80 ||
                (data[i + 1] & 0xc0) != 0x80)
                throw std::runtime_error("invalid modified UTF-8 three-byte sequence");
            const auto second = data[i++];
            const auto third = data[i++];
            units.push_back(static_cast<std::uint16_t>(((first & 0x0f) << 12) |
                ((second & 0x3f) << 6) | (third & 0x3f)));
        } else {
            throw std::runtime_error("invalid modified UTF-8 leading byte");
        }
    }

    std::string result;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto unit = units[i];
        if (unit >= 0xd800 && unit <= 0xdbff && i + 1 < units.size() &&
            units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
            const auto codepoint = 0x10000u +
                ((std::uint32_t(unit - 0xd800) << 10) | (units[++i] - 0xdc00));
            append_utf8(result, codepoint);
        } else if (unit >= 0xd800 && unit <= 0xdfff) {
            // Match langfile.py's UTF-16 replacement behavior for unpaired surrogates.
            append_utf8(result, 0xfffd);
        } else {
            append_utf8(result, unit);
        }
    }
    return result;
}
}

std::vector<std::string> parse_language_bank(const std::vector<std::uint8_t>& bytes) {
    const auto payload_size = static_cast<std::size_t>(read_u32_be(bytes, 0));
    if (payload_size != bytes.size() - 4)
        throw std::runtime_error("language-bank payload length mismatch");
    const auto blob_begin = std::size_t{4};
    const auto blob_size = payload_size;
    if (blob_size < 4) throw std::runtime_error("language bank has no offset table");
    const auto first_offset = static_cast<std::size_t>(read_u32_be(bytes, blob_begin));
    if (first_offset > blob_size - 4)
        throw std::runtime_error("language-bank first string offset outside payload");
    const auto first_string = std::size_t{4} + first_offset;
    if (first_string > blob_size || first_string % 4 != 0)
        throw std::runtime_error("invalid language-bank offset table size");
    const auto count = first_string / 4;
    if (count > blob_size / 4) throw std::runtime_error("language-bank offset table exceeds data");

    std::vector<std::string> strings;
    strings.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = static_cast<std::size_t>(read_u32_be(bytes, blob_begin + i * 4));
        if (offset > blob_size || i * 4 + 4 > blob_size - offset)
            throw std::runtime_error("language string offset outside payload");
        const auto length_pos = blob_begin + i * 4 + 4 + offset + 2;
        const auto length = static_cast<std::size_t>(read_u16_be(bytes, length_pos));
        if (length_pos > bytes.size() || bytes.size() - length_pos < 2 ||
            length > bytes.size() - length_pos - 2)
            throw std::runtime_error("language string exceeds payload");
        strings.push_back(decode_modified_utf8(bytes.data() + length_pos + 2, length));
    }
    return strings;
}

std::vector<std::string> load_language_bank(const GameArchive& archive,
                                            const std::string& path) {
    return parse_language_bank(archive.read_entry(path));
}

std::optional<std::string> resolve_event_text(
    const EventGrid& event, std::size_t event_string_index,
    const std::vector<std::string>& language_bank) {
    if (event_string_index >= event.strings.size()) return std::nullopt;
    const auto& reference = event.strings[event_string_index];
    if (reference.empty()) return std::nullopt;
    std::size_t language_index = 0;
    const auto* begin = reinterpret_cast<const char*>(reference.data());
    const auto* end = begin + reference.size();
    const auto parsed = std::from_chars(begin, end, language_index);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        language_index >= language_bank.size()) return std::nullopt;
    return format_dialogue_text(language_bank[language_index]);
}

std::string format_game_text(std::string text) {
    for(auto& character:text) if(character==';') character='\n';
    return text;
}

std::vector<std::string> parse_tdf_text(const std::vector<std::uint8_t>& bytes,
    const std::vector<std::string>& language_bank) {
    if(bytes.empty()) throw std::runtime_error("truncated TDF count");
    std::vector<std::string> text;
    std::size_t pos=1;
    for(unsigned i=0;i<bytes[0];++i) {
        const auto length=read_u16_be(bytes,pos);pos+=2;
        if(pos>bytes.size() || length>bytes.size()-pos) throw std::runtime_error("truncated TDF reference");
        const char* begin=reinterpret_cast<const char*>(bytes.data()+pos);
        const char* end=begin+length;
        while(begin<end && static_cast<unsigned char>(*begin)<=32) ++begin;
        while(end>begin && static_cast<unsigned char>(end[-1])<=32) --end;
        int key=-1;const auto parsed=std::from_chars(begin,end,key);
        if(parsed.ec!=std::errc{} || parsed.ptr!=end || key<0 || std::size_t(key)>=language_bank.size())
            throw std::runtime_error("invalid TDF language reference");
        text.push_back(format_game_text(language_bank[key]));pos+=length;
    }
    if(pos!=bytes.size()) throw std::runtime_error("unexpected TDF trailing bytes");
    return text;
}

std::string format_dialogue_text(std::string text) {
    text=format_game_text(std::move(text));
    // Speaker/style prefixes in the language bank are not visible dialogue glyphs.
    if(text.size()>1 && text[1]=='[' && (text[0]=='|' || text[0]=='$' || text[0]=='#' || text[0]=='?'))
        text.erase(0,1);
    return text;
}

std::optional<std::size_t> event_text_bank_index(const EventGrid& event, std::size_t event_string_index) {
    if (event_string_index >= event.strings.size()) return std::nullopt;
    const auto& reference = event.strings[event_string_index];
    std::size_t language_index = 0;
    const auto* begin = reinterpret_cast<const char*>(reference.data());
    const auto* end = begin + reference.size();
    const auto parsed = std::from_chars(begin, end, language_index);
    if (reference.empty() || parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return language_index;
}
