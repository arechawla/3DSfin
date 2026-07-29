#pragma once
#include <string>

// Plays a Jellyfin MPEG-TS/H.264 stream using the New 3DS MVD hardware decoder.
// Blocks until the stream ends or the user presses B.
// runTimeTicks is the item's total duration (10,000,000 ticks/sec, 0 if unknown)
// and drives the bottom-screen seek bar. series/title/year are shown above it.
// startSec is the resume offset (seconds) the stream was seeked to server-side;
// it's added to the PTS-derived position so the seek bar reads the true time.
// A live transcode stream can't be seeked client-side, so D-Pad Left/Right
// (-10s/+30s) end playback and report the target via *seekOut (-1 = no seek);
// the caller then requests a fresh stream at that position and calls back in.
// Returns false if MVD init fails (Old 3DS or allocation error).
bool playerPlay(const std::string& url, long long runTimeTicks = 0,
                const std::string& series = "",
                const std::string& title  = "",
                int year = 0,
                double startSec = 0.0,
                double* seekOut = nullptr);
