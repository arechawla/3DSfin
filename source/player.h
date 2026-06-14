#pragma once
#include <string>

// Plays a Jellyfin MPEG-TS/H.264 stream using the New 3DS MVD hardware decoder.
// Blocks until the stream ends or the user presses B.
// Returns false if MVD init fails (Old 3DS or allocation error).
bool playerPlay(const std::string& url);
