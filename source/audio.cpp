#include "audio.h"
#include <3ds.h>
#include <cstring>

namespace audio {

// ─── Configuration ────────────────────────────────────────────────────────────
// One AAC frame decodes to 1024 PCM samples (2048 with SBR upsampling). Size each
// wave buffer for the worst case (2048 stereo frames) so a single decoded frame
// always fits. NUM_WBUF buffers give several hundred ms of queued lookahead: the
// steady-state queue depth is only ~AV_LEAD_MS (the pacer holds video that far
// ahead of audio playback), but demux bursts (startup, post-stall catch-up) queue
// well past it and every drop is an audible gap, so keep generous headroom.
static constexpr int   NUM_WBUF      = 32;
static constexpr int   MAX_FRAMES    = 2048;        // per-channel samples per buffer
static constexpr int   MAX_CHANS     = 2;
static constexpr u32   WBUF_BYTES    = MAX_FRAMES * MAX_CHANS * sizeof(int16_t);
static constexpr int   AUDIO_CHN     = 0;           // ndsp channel id

static bool          g_enabled    = false;          // ndspInit succeeded
static bool          g_configured = false;          // channel set up for a format
static int           g_rate       = 0;
static int           g_chans      = 0;
static ndspWaveBuf   g_wbuf[NUM_WBUF];
static int16_t*      g_mem[NUM_WBUF] = {0};
static double        g_bufPts[NUM_WBUF];            // PTS (sec) of each buffer's first sample
static int           g_next       = 0;              // round-robin search start
static unsigned      g_dropped    = 0;              // blocks dropped since init()

bool init() {
    if (g_enabled) return true;
    if (R_FAILED(ndspInit())) return false;          // DSP firm missing → no audio
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    for (int i = 0; i < NUM_WBUF; i++) {
        g_mem[i] = (int16_t*)linearAlloc(WBUF_BYTES);
        if (!g_mem[i]) {                             // out of linear mem → bail cleanly
            for (int j = 0; j < i; j++) { linearFree(g_mem[j]); g_mem[j] = nullptr; }
            ndspExit();
            return false;
        }
        memset(&g_wbuf[i], 0, sizeof(ndspWaveBuf));
        g_wbuf[i].data_pcm16 = g_mem[i];
        g_wbuf[i].status     = NDSP_WBUF_DONE;       // mark free for the first push
        g_bufPts[i]          = -1.0;
    }
    g_dropped = 0;
    g_enabled = true;
    return true;
}

void configure(int sampleRate, int channels) {
    if (!g_enabled) return;
    if (channels < 1) channels = 1;
    if (channels > MAX_CHANS) channels = MAX_CHANS;
    if (g_configured && sampleRate == g_rate && channels == g_chans) return;

    ndspChnReset(AUDIO_CHN);
    ndspChnSetInterp(AUDIO_CHN, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHN, (float)sampleRate);
    ndspChnSetFormat(AUDIO_CHN,
                     channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                                   : NDSP_FORMAT_MONO_PCM16);

    // Full volume on the front-left/right mix lanes.
    float mix[12] = {0};
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(AUDIO_CHN, mix);

    g_rate = sampleRate;
    g_chans = channels;
    g_configured = true;
}

bool ready() { return g_enabled && g_configured; }

void push(const int16_t* pcm, int samplesPerChan, int channels, double ptsSec) {
    if (!g_configured || samplesPerChan <= 0) return;
    if (channels < 1) channels = 1;
    if (channels > MAX_CHANS) channels = MAX_CHANS;
    if (samplesPerChan > MAX_FRAMES) samplesPerChan = MAX_FRAMES;

    // Find a wave buffer the DSP has finished with (or never used yet).
    int idx = -1;
    for (int i = 0; i < NUM_WBUF; i++) {
        int k = (g_next + i) % NUM_WBUF;
        if (g_wbuf[k].status == NDSP_WBUF_DONE || g_wbuf[k].status == NDSP_WBUF_FREE) {
            idx = k;
            break;
        }
    }
    if (idx < 0) { g_dropped++; return; }             // all queued → drop this block
    g_next = (idx + 1) % NUM_WBUF;

    u32 bytes = (u32)samplesPerChan * channels * sizeof(int16_t);
    memcpy(g_mem[idx], pcm, bytes);
    DSP_FlushDataCache(g_mem[idx], bytes);

    g_wbuf[idx].data_pcm16 = g_mem[idx];
    g_wbuf[idx].nsamples   = samplesPerChan;         // per-channel frame count
    g_wbuf[idx].looping    = false;
    g_bufPts[idx]          = ptsSec;
    ndspChnWaveBufAdd(AUDIO_CHN, &g_wbuf[idx]);
}

int queuedBufs() {
    int n = 0;
    for (int i = 0; i < NUM_WBUF; i++)
        if (g_wbuf[i].status == NDSP_WBUF_QUEUED || g_wbuf[i].status == NDSP_WBUF_PLAYING)
            n++;
    return n;
}

unsigned droppedBlocks() { return g_dropped; }

void flushQueue() {
    if (!g_enabled) return;
    ndspChnWaveBufClear(AUDIO_CHN);
    // After the clear ndsp forgets these buffers, so it will never flip their
    // status back to DONE itself — mark them free by hand. Worst case the DSP is
    // mid-mix on one of them and a subsequent push overwrites it: one brief
    // glitch, on a path that runs at most once per playback.
    for (int i = 0; i < NUM_WBUF; i++) {
        g_wbuf[i].status = NDSP_WBUF_DONE;
        g_bufPts[i]      = -1.0;
    }
}

void setPaused(bool paused) {
    if (!g_enabled) return;
    ndspChnSetPaused(AUDIO_CHN, paused);
}

double audioClock() {
    if (!g_configured || g_rate <= 0) return -1.0;
    // The ndsp thread flips buffer status asynchronously; a read racing a buffer
    // transition can pair the sample position with the wrong buffer's PTS, but the
    // resulting error is bounded by one buffer (~20-40 ms) and the caller only uses
    // this clock through a heavily damped correction, so no locking is needed.
    for (int i = 0; i < NUM_WBUF; i++) {
        if (g_wbuf[i].status == NDSP_WBUF_PLAYING) {
            if (g_bufPts[i] < 0) return -1.0;        // buffer pushed without a PTS
            return g_bufPts[i] + (double)ndspChnGetSamplePos(AUDIO_CHN) / (double)g_rate;
        }
    }
    return -1.0;                                     // idle/underrun: no sample playing
}

void exit() {
    if (!g_enabled) return;
    ndspChnWaveBufClear(AUDIO_CHN);
    ndspExit();
    for (int i = 0; i < NUM_WBUF; i++) {
        if (g_mem[i]) { linearFree(g_mem[i]); g_mem[i] = nullptr; }
    }
    g_enabled = false;
    g_configured = false;
    g_rate = g_chans = 0;
    g_next = 0;
}

}  // namespace audio
