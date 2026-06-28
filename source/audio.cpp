#include "audio.h"
#include <3ds.h>
#include <cstring>

namespace audio {

// ─── Configuration ────────────────────────────────────────────────────────────
// One AAC frame decodes to 1024 PCM samples (2048 with SBR upsampling). Size each
// wave buffer for the worst case (2048 stereo frames) so a single decoded frame
// always fits. NUM_WBUF buffers give a few hundred ms of queued lookahead, which
// covers the gaps while the main thread is busy pacing/blitting video.
static constexpr int   NUM_WBUF      = 16;
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
static int           g_next       = 0;              // round-robin search start

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
    }
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

void push(const int16_t* pcm, int samplesPerChan, int channels) {
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
    if (idx < 0) return;                              // all queued → drop this block
    g_next = (idx + 1) % NUM_WBUF;

    u32 bytes = (u32)samplesPerChan * channels * sizeof(int16_t);
    memcpy(g_mem[idx], pcm, bytes);
    DSP_FlushDataCache(g_mem[idx], bytes);

    g_wbuf[idx].data_pcm16 = g_mem[idx];
    g_wbuf[idx].nsamples   = samplesPerChan;         // per-channel frame count
    g_wbuf[idx].looping    = false;
    ndspChnWaveBufAdd(AUDIO_CHN, &g_wbuf[idx]);
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
