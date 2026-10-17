// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

/// The console's languages, by the numbers a title is told (sceSystemServiceParamGetInt with
/// ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG).
inline constexpr std::array<const char*, 30> ConsoleLanguageNames{
    "Japanese",
    "English (United States)",
    "French",
    "Spanish",
    "German",
    "Italian",
    "Dutch",
    "Portuguese (Portugal)",
    "Russian",
    "Korean",
    "Chinese (traditional)",
    "Chinese (simplified)",
    "Finnish",
    "Swedish",
    "Danish",
    "Norwegian",
    "Polish",
    "Portuguese (Brazil)",
    "English (United Kingdom)",
    "Turkish",
    "Spanish (Latin America)",
    "Arabic",
    "French (Canada)",
    "Czech",
    "Hungarian",
    "Greek",
    "Romanian",
    "Thai",
    "Vietnamese",
    "Indonesian",
};

inline const char* ConsoleLanguageName(int language) {
    return language >= 0 && language < static_cast<int>(ConsoleLanguageNames.size())
               ? ConsoleLanguageNames[language]
               : "unknown";
}

/// The console's number for a language named the way the systems around the emulator name
/// theirs: a language tag ("fr-FR", "pt_BR", "zh-Hans-CN", "es-419"), or the number itself.
/// Nothing for what the console has no language for.
inline std::optional<int> ConsoleLanguageFromTag(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    bool digits = true;
    for (const char c : text) {
        digits = digits && std::isdigit(static_cast<unsigned char>(c));
    }
    if (digits) {
        if (text.size() > 2) {
            return std::nullopt;
        }
        int number = 0;
        for (const char c : text) {
            number = number * 10 + (c - '0');
        }
        return number < static_cast<int>(ConsoleLanguageNames.size()) ? std::optional{number}
                                                                      : std::nullopt;
    }

    // The language, and what follows it (script, region; "fr_FR.UTF-8" has more).
    std::vector<std::string> parts{{}};
    for (const char c : text) {
        if (c == '.' || c == '@') {
            break;
        }
        if (c == '-' || c == '_') {
            parts.emplace_back();
        } else {
            parts.back().push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    const std::string& language = parts.front();
    const auto has = [&](std::string_view part) {
        for (size_t i = 1; i < parts.size(); ++i) {
            if (parts[i] == part) {
                return true;
            }
        }
        return false;
    };
    const bool regional = parts.size() > 1;

    if (language == "en") {
        // British spelling everywhere but in North America.
        return !regional || has("us") || has("ca") || has("ph") ? 1 : 18;
    }
    if (language == "fr") {
        return has("ca") ? 22 : 2;
    }
    if (language == "es") {
        return !regional || has("es") ? 3 : 20;
    }
    if (language == "pt") {
        return has("br") ? 17 : 7;
    }
    if (language == "zh") {
        return has("hant") || has("tw") || has("hk") || has("mo") ? 10 : 11;
    }
    struct Entry {
        std::string_view language;
        int number;
    };
    static constexpr Entry Others[] = {
        {"ja", 0},  {"de", 4},  {"it", 5},  {"nl", 6},  {"ru", 8},  {"ko", 9},  {"fi", 12},
        {"sv", 13}, {"da", 14}, {"nb", 15}, {"no", 15}, {"nn", 15}, {"pl", 16}, {"tr", 19},
        {"ar", 21}, {"cs", 23}, {"hu", 24}, {"el", 25}, {"ro", 26}, {"th", 27}, {"vi", 28},
        {"id", 29}, {"in", 29},
    };
    for (const Entry& entry : Others) {
        if (language == entry.language) {
            return entry.number;
        }
    }
    return std::nullopt;
}

} // namespace Common
