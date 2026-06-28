# Audio Follow-ups

Deferred work on the audio pipeline. Initial AAC audio playback (Helix decoder +
ndsp output) is implemented and working; these are the known remaining items.

Background on the current pipeline:
- `source/aac/` — vendored Helix AAC decoder (fixed-point, ARM). Decodes AAC-LC
  ADTS frames to interleaved PCM16.
- `source/audio.{h,cpp}` — thin `ndsp` wrapper: one stereo PCM16 channel, a ring
  of 16 wave buffers in linear memory, lazy `configure()` from the first frame's
  rate/channels, `push()` that drops rather than stalls when buffers are full.
- `source/player.cpp` — `parsePMT()` now returns the AAC PID (stream type 0x0F /
  0x11); a second PES accumulator (`audBuf`/`audLen`/`audActive`) collects audio
  packets; `processAAC()` finds ADTS syncwords, decodes with Helix, and queues
  PCM to ndsp.

---

## 1. A/V sync (audio-clock master)

**Status:** deferred. Audio currently runs free.

**Current behavior:** Audio plays through the DSP at its own sample rate while
video is paced independently by `paceToPts()` (which anchors wall-clock time to
the first displayed frame's PTS — see `source/player.cpp`). Both the audio and
video elementary streams are drained from the same MPEG-TS ring at roughly
real time, so in practice they track each other closely. But there is no actual
synchronization: nothing corrects drift between the audio sample clock and the
video PTS clock, so over a long playback they can slowly separate.

**Why this is the right next step:** On essentially every media player, **audio is
the master clock** — the ear notices audio glitches (gaps, pitch changes from
resampling) far more than a video frame shown a few ms early/late. The DSP plays
samples at a fixed, hardware-accurate rate, which makes it the most reliable clock
we have. Video should be slaved to it.

**Implementation approach:**
- Track how many audio samples the DSP has actually played. Options:
  - Maintain a running count of samples *queued* and read back consumed buffers,
    or
  - Use `ndspChnGetSamplePos(channel)` to read the current playback position.
- Convert the played-sample count to an audio wall-clock in the same 90 kHz units
  the video PTS uses: `audioClock90k = samplesPlayed * 90000 / sampleRate`.
- In `paceToPts()` (or a replacement), instead of pacing video against
  `osGetTime()`, pace each video frame's PTS against the **audio clock**:
  - If the frame's PTS is ahead of the audio clock → sleep until audio catches up.
  - If it's behind by more than a threshold → drop the frame (or show immediately)
    to catch up, rather than the current "re-anchor wall clock" resync.
- Keep the existing wall-clock pacing as a fallback for the window before the
  first audio frame is decoded / when audio is disabled (no `dsp_firm`).

**Gotchas:**
- The audio and video PTS share the same 90 kHz timebase from the TS, but their
  *origins* differ — capture the first audio PTS and first video PTS and offset
  accordingly, or anchor both to the first decoded audio sample.
- Handle the audio-disabled case (DSP firmware missing): fall back cleanly to the
  current wall-clock pacing so video still plays smoothly.
- `processAAC()` does not currently track audio PTS. To sync, we'll need to thread
  the audio PES PTS (already parsed by `pesPTS()` for video) into the audio path.

---

## 2. Final-PES audio flush

**Status:** deferred (minor).

**Current behavior:** `processAAC()` is called to decode an accumulated audio PES
only when the *next* audio PES begins (on PUSI), mirroring how the video path
flushes. This means the very last audio PES of a stream — the one with no
following PUSI — is never decoded. At end-of-stream we lose roughly one PES worth
of audio (~tens of ms).

**Why it's low priority:** It's a single short gap right as playback ends, almost
always inaudible. It was left out to keep the initial diff small.

**Implementation approach:** After the main decode loop in `playerPlay()` exits
(stream finished / user pressed B), before teardown, add:

```c
if (audActive && audLen > 0)
    processAAC(audBuf, (int)audLen, dbg);
```

Place it alongside the existing end-of-stream handling, before
`AACFreeDecoder()` / `audio::exit()`. Symmetric with how the video path could
also flush its final `pesBuf` if we ever care about the last video frame.
