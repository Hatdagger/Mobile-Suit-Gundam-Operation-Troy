// The player's name, shown in the game and to other players online. The game
// reads it from the "gamertag" line of gundam_operation_troy.toml.
#pragma once

#include <string>
#include <string_view>

namespace player_name {

constexpr size_t kMaxLength = 15;            // the Xbox 360's gamertag limit
constexpr const char* kDefault = "Player";   // what the game uses when none is set
constexpr const char* kConfigKey = "gamertag";

// Letters, digits, space, '-', '_' and '.'. These need no escaping in the
// settings file, and other players' games can show them.
bool Allowed(char c);

// What typing `c` after `current` gives: the same text if `c` is not allowed,
// the name is full, or it would start with a space or make two spaces in a row.
std::string Type(std::string_view current, char c);

// Keeps the allowed characters, joins runs of spaces, trims the ends and cuts
// to kMaxLength. Empty means "no name".
std::string Clean(std::string_view text);

}  // namespace player_name
