#pragma once
#include <cstdint>

// Thin wrapper over libctru's ndsp (DSP audio service) for streaming PCM16.
//
// Lifecycle: audioInit() once before playback; audioConfigure() with the real
// sample rate/channel count once the first decoded AAC frame reveals them;
// audioPush() per decoded frame; audioExit() when playback ends.
//
// audioInit() returns false if the DSP firmware isn't available (dsp_firm not
// dumped to the SD card). The caller should then simply play video without
// sound rather than treating it as fatal.

namespace audio {

// Initialise ndsp. Returns false if the DSP firmware is missing/unavailable.
bool init();

// Configure the output channel for the given PCM format. Safe to call again if
// the stream's rate/channels change; a no-op when they match the current setup.
// Does nothing if init() failed.
void configure(int sampleRate, int channels);

// Queue one block of interleaved signed-16-bit PCM. samplesPerChan is the number
// of sample frames (not total samples). If no wave buffer is free the block is
// dropped (a brief audio gap is preferable to stalling video). No-op until
// configure() has run.
void push(const int16_t* pcm, int samplesPerChan, int channels);

// True if audio is initialised and a channel is configured (i.e. push() works).
bool ready();

// Stop playback and release ndsp + buffers.
void exit();

}  // namespace audio
