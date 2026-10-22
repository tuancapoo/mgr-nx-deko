// Sonic Adventure 2: the game's language (sa2.toml "language"), see sa2_language.cpp.

#pragma once

namespace sa2::language {

// Sets the SDK's user_language from the "language" setting ("auto" = the console's / PC's language). Before any guest
// code runs: the game asks for it at start (XGetLanguage).
void Apply();

}  // namespace sa2::language
