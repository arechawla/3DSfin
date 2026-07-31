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
// of sample frames (not total samples). ptsSec is the program-clock presentation
// time (seconds) of the first sample in the block, or negative if unknown; it
// tags the wave buffer so audioClock() can report what is playing. If no wave
// buffer is free the block is dropped (a brief audio gap is preferable to
// stalling video) — the clock stays correct because later buffers carry their
// own PTS. No-op until configure() has run.
void push(const int16_t* pcm, int samplesPerChan, int channels, double ptsSec);

// Program-clock time (seconds) of the audio sample the DSP is playing right now:
// the PTS tag of the currently playing wave buffer plus the DSP's sample position
// within it. Returns a negative value when unavailable (audio disabled, nothing
// playing yet, or the playing buffer was pushed without a PTS).
double audioClock();

// True if audio is initialised and a channel is configured (i.e. push() works).
bool ready();

// Wave buffers currently queued or playing (the audio feed cushion), and the
// number of blocks dropped since init() because no buffer was free. Both are
// diagnostics for tuning the video pacer's audio-clock servo.
int queuedBufs();
unsigned droppedBlocks();

// Discard everything queued but not yet played (e.g. stale audio a resumed
// transcode primed before the video start). Playback continues with the next
// push(). No-op if init() failed.
void flushQueue();

// Halt/resume DSP playback without touching the queue. While paused the channel's
// sample position is frozen, so audioClock() holds its value and the video pacer's
// audio servo stays consistent across the pause. No-op if init() failed.
void setPaused(bool paused);

// Stop playback and release ndsp + buffers.
void exit();

}  // namespace audio
