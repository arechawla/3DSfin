#include "player.h"
#include <3ds.h>
#include <cstring>
#include <cstdio>

// ─── Dimensions ──────────────────────────────────────────────────────────────
// VID_W/VID_H are the MAX (output buffer is allocated for these). The actual
// coded resolution is read from the H.264 SPS at runtime — Jellyfin preserves
// aspect ratio under the MaxWidth/MaxHeight caps, so the real frame is often
// shorter than 240 (e.g. 400x224 for 16:9). MVD must be configured with the
// real coded dims or render() silently writes nothing.
static constexpr u32 VID_W = 400, VID_H = 240;
static constexpr u32 FB_W  = 240, FB_H  = 400;

// Actual coded dimensions (updated from SPS; default to the max until parsed).
static u32 g_decW = VID_W, g_decH = VID_H;

// On-screen debug overlay (bottom-screen console + green test paint). Hidden by
// default; toggled during playback by holding X + D-Pad Up. File logging to
// player_debug.txt is independent of this flag. DBG() prints only when enabled.
static bool g_dbg = false;
#define DBG(...) do { if (g_dbg) printf(__VA_ARGS__); } while (0)

// ─── Buffer sizes ─────────────────────────────────────────────────────────────
static constexpr u32 TS_SZ  = 188;
static constexpr u32 RD_SZ  = TS_SZ * 128;
static constexpr u32 PES_SZ = 512 * 1024;
static constexpr u32 NAL_SZ = 512 * 1024;

// ─── MPEG-TS helpers ──────────────────────────────────────────────────────────
static inline int  tsPid (const u8* p) { return ((p[1]&0x1F)<<8)|p[2]; }
static inline bool tsPUSI(const u8* p) { return (p[1]&0x40)!=0; }

static const u8* tsPayload(const u8* p, int* outSz) {
    u8 afc = (p[3]>>4)&3;
    if (afc == 2) { *outSz=0; return nullptr; }
    int off = 4;
    if (afc == 3) off += 1 + p[4];
    if (off >= (int)TS_SZ) { *outSz=0; return nullptr; }
    *outSz = TS_SZ - off;
    return p + off;
}

// ─── PAT parser ───────────────────────────────────────────────────────────────
static int parsePAT(const u8* pl, int sz) {
    if (sz < 9) return -1;
    int ptr = pl[0];
    const u8* t = pl + 1 + ptr;
    int rem = sz - 1 - ptr;
    if (rem < 8 || t[0] != 0x00) return -1;
    int secLen     = ((t[1]&0x0F)<<8)|t[2];
    int entryBytes = secLen - 9;
    const u8* prg  = t + 8;
    for (int i = 0; i+4 <= entryBytes && i+4 <= rem-8; i += 4) {
        int prog = (prg[i]<<8)|prg[i+1];
        int pid  = ((prg[i+2]&0x1F)<<8)|prg[i+3];
        if (prog != 0) return pid;
    }
    return -1;
}

// ─── PMT parser ───────────────────────────────────────────────────────────────
static int parsePMT(const u8* pl, int sz) {
    if (sz < 13) return -1;
    int ptr = pl[0];
    const u8* t = pl + 1 + ptr;
    int rem = sz - 1 - ptr;
    if (rem < 12 || t[0] != 0x02) return -1;
    int secLen  = ((t[1]&0x0F)<<8)|t[2];
    int piLen   = ((t[10]&0x0F)<<8)|t[11];
    int esBytes = secLen - 13 - piLen;
    const u8* es = t + 12 + piLen;
    int remEs    = rem - 12 - piLen;
    for (int o = 0; o+5 <= esBytes && o+5 <= remEs; ) {
        int type = es[o];
        int pid  = ((es[o+1]&0x1F)<<8)|es[o+2];
        int esil = ((es[o+3]&0x0F)<<8)|es[o+4];
        if (type == 0x1B) return pid;
        o += 5 + esil;
    }
    return -1;
}

// ─── PES header skip ──────────────────────────────────────────────────────────
static int pesHeaderLen(const u8* pay, int sz) {
    if (sz < 9 || pay[0]!=0 || pay[1]!=0 || pay[2]!=1) return 0;
    return 9 + pay[8];
}

// Extract the 90 kHz presentation timestamp from a PES header, or -1 if absent.
static long long pesPTS(const u8* pay, int sz) {
    if (sz < 14 || pay[0]!=0 || pay[1]!=0 || pay[2]!=1) return -1;
    if (!(pay[7] & 0x80)) return -1;             // PTS_DTS_flags: no PTS present
    return ((long long)((pay[9]  >> 1) & 0x07) << 30)
         | ((long long) pay[10]               << 22)
         | ((long long)((pay[11] >> 1) & 0x7F) << 15)
         | ((long long) pay[12]               <<  7)
         | ((long long)((pay[13] >> 1) & 0x7F));
}

// ─── Seek bar (bottom-screen text console) ───────────────────────────────────
static void fmtTime(char* buf, size_t n, double sec) {
    if (sec < 0) sec = 0;
    int t = (int)(sec + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0) snprintf(buf, n, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, n, "%02d:%02d", m, s);
}

// Draws "MM:SS / MM:SS" and a [####----] bar near the bottom of the console.
// Uses ANSI cursor positioning so it updates in place (no scrolling).
static void drawSeekBar(double posSec, double durSec) {
    const int barW = 28;
    char bar[barW + 1];
    int filled = 0;
    if (durSec > 0) {
        double frac = posSec / durSec;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        filled = (int)(frac * barW + 0.5);
    }
    for (int i = 0; i < barW; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[barW] = '\0';

    char cur[16];
    fmtTime(cur, sizeof(cur), posSec);
    if (durSec > 0) {
        char tot[16];
        fmtTime(tot, sizeof(tot), durSec);
        printf("\x1b[27;5H%s / %s    ", cur, tot);   // trailing spaces clear leftovers
    } else {
        printf("\x1b[27;5H%s    ", cur);             // unknown duration
    }
    printf("\x1b[29;5H[%s]", bar);
}

// Series name / episode title / year above the seek bar, one blank line between
// each item. A long title wraps to a second line instead of being truncated.
// The block is bottom-anchored so it always ends just above the seek bar.
static void drawMeta(const std::string& series, const std::string& title, int year) {
    const size_t W = 34;   // console text width (col 5 .. 38)

    // Wrap the title into up to two lines.
    std::string t1, t2;
    if (title.size() <= W) {
        t1 = title;
    } else {
        size_t len = W;
        size_t sp  = title.rfind(' ', W);
        if (sp != std::string::npos && sp > 0) len = sp;   // break on a space
        t1 = title.substr(0, len);
        size_t j = len;
        while (j < title.size() && title[j] == ' ') j++;
        t2 = title.substr(j);
        if (t2.size() > W) t2 = t2.substr(0, W - 3) + "...";
    }

    int titleLines = title.empty() ? 0 : (t2.empty() ? 1 : 2);
    int present    = (series.empty() ? 0 : 1) + (title.empty() ? 0 : 1) + (year > 0 ? 1 : 0);
    int contentRows = (series.empty() ? 0 : 1) + titleLines + (year > 0 ? 1 : 0);
    int total = contentRows + (present > 0 ? present - 1 : 0);   // + blank between items

    int row = 25 - total + 1;   // last line lands on row 25 (blank row 26, bar at 27)
    if (row < 1) row = 1;

    if (!series.empty()) { printf("\x1b[%d;5H%.34s", row, series.c_str()); row += 2; }
    if (!title.empty()) {
        printf("\x1b[%d;5H%s", row, t1.c_str()); row++;
        if (!t2.empty()) { printf("\x1b[%d;5H%s", row, t2.c_str()); row++; }
        row++;   // blank line after the title
    }
    if (year > 0) printf("\x1b[%d;5H%d", row, year);
}

// ─── Blit 400x240 BGR565 -> 240x400 BGR8 and swap ────────────────────────────
// 3DS top-screen layout: column-major, 240 rows per column.
// Logical (x,y) -> physical offset = (x*240 + (239-y)) * 3
// C3D_Fini is called in main before playerPlay, so gspWaitForVBlank is safe.
// Only swap the top screen to avoid disturbing the console on the bottom screen.
static void blitFrame(const u8* mvdOut, u32 frameCount) {
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    const u16* src = (const u16*)mvdOut;

    if (frameCount < 3) {
        DBG("fb=%p px[0]=%04X dim=%lux%lu\n",
            (void*)fb, (unsigned)src[0],
            (unsigned long)g_decW, (unsigned long)g_decH);
    }

    // Black out the screen first so a smaller-than-screen frame has no leftover
    // (e.g. the green test paint) in the letterbox margin.
    memset(fb, 0, FB_W * FB_H * 3);

    // Source stride is the coded width (g_decW); clamp to screen bounds.
    u32 maxY = (g_decH < FB_W) ? g_decH : FB_W;   // y maps to screen X (240 wide)
    u32 maxX = (g_decW < FB_H) ? g_decW : FB_H;   // x maps to screen Y (400 tall)
    for (u32 y = 0; y < maxY; y++) {
        for (u32 x = 0; x < maxX; x++) {
            u16 px  = src[y * g_decW + x];
            u8  r5  = (px >> 11) & 0x1F;
            u8  g6  = (px >> 5)  & 0x3F;
            u8  b5  =  px        & 0x1F;
            u32 off = (x * FB_W + (FB_W - 1 - y)) * 3;
            fb[off]     = (b5 << 3) | (b5 >> 2);
            fb[off + 1] = (g6 << 2) | (g6 >> 4);
            fb[off + 2] = (r5 << 3) | (r5 >> 2);
        }
    }

    GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
    gspWaitForVBlank();
    gfxScreenSwapBuffers(GFX_TOP, false);
}

// Mirrors to the on-screen console (only when debug is enabled) and always to file.
#define DLOG(dbg, ...) do { if (g_dbg) printf(__VA_ARGS__); if(dbg){fprintf(dbg,__VA_ARGS__);fflush(dbg);} } while(0)

// ─── PTS-based frame pacing ───────────────────────────────────────────────────
// Without pacing, frames blit as fast as they decode, which is bursty: HTTP
// delivers data in chunks, each holding several frames, so playback fast-forwards
// through a burst (one vblank per frame) then freezes waiting for the next chunk.
// We anchor wall-clock time to the first displayed frame's PTS and, before every
// later frame, sleep until its scheduled presentation time. When decode can't keep
// up (now is already past target) nothing sleeps, so we never play slower than the
// hardware allows — pacing only smooths the too-fast bursts.
static long long g_pacePts0  = -1;   // 90 kHz PTS of the first displayed frame
static u64       g_paceWall0 = 0;    // osGetTime() (ms) captured at that first frame
static void paceToPts(long long auPts) {
    if (auPts < 0) return;                            // no PTS on this AU → can't pace
    if (g_pacePts0 < 0) {                             // first frame: set the anchor
        g_pacePts0 = auPts; g_paceWall0 = osGetTime();
        return;
    }
    long long dpts = auPts - g_pacePts0;
    if (dpts < 0) dpts += (1LL << 33);                // 33-bit PTS wraparound
    u64 target = g_paceWall0 + (u64)(dpts / 90);      // 90 kHz ticks → ms
    u64 now    = osGetTime();
    if (target > now) {                               // ahead of schedule → wait
        u64 waitMs = target - now;
        if (waitMs > 1000) waitMs = 1000;             // cap: PTS-discontinuity safety
        DBG("pace %lums\n", (unsigned long)waitMs);
        svcSleepThread((s64)waitMs * 1000000LL);
    } else if (now - target > 200) {
        // Fell well behind (network stall starved the decoder). Don't race to catch
        // up — that dumps the backlog at max speed and looks like a fast-forward
        // shake. Re-anchor the clock to "now" and resume smooth pacing from here.
        g_pacePts0 = auPts; g_paceWall0 = now;
        DBG("pace resync\n");
    }
}

// ─── Read-ahead download ring buffer ──────────────────────────────────────────
// A background thread continuously pulls the HTTP TS stream into this ring while
// the main thread demuxes/decodes/paces out of it. This decouples network I/O
// from display timing: when paceToPts() sleeps to hold a frame to its real
// presentation time, the download thread keeps the buffer full instead of the
// socket starving — which is what previously caused the stutter (a single thread
// can't both pace display AND keep reading). RING_SZ is a power of two so the
// free-running u32 head/tail counters wrap cleanly; index = pos & RING_MASK.
static constexpr u32 RING_SZ   = 8u * 1024 * 1024;   // ~1.8 min at 0.6 Mbps
static constexpr u32 RING_MASK = RING_SZ - 1;
static constexpr u32 PREBUF_SZ = 512u * 1024;        // fill this much before playing
static constexpr u32 DL_CHUNK  = 128u * 1024;        // bytes per httpcDownloadData call

struct DlRing {
    u8*           data;
    httpcContext* ctx;
    volatile u32  head;          // producer: total bytes written
    volatile u32  tail;          // consumer: total bytes consumed
    volatile bool producerDone;  // stream ended/errored — no more data coming
    volatile bool consumerStop;  // consumer asked the producer to stop
    LightLock     lock;          // guards the head/tail snapshot
};
static DlRing g_ring;

static inline u32 ringUsed() {
    LightLock_Lock(&g_ring.lock);
    u32 u = g_ring.head - g_ring.tail;   // wrap-safe: both are free-running u32
    LightLock_Unlock(&g_ring.lock);
    return u;
}

// Producer thread: download HTTP into the ring until the stream ends or the
// consumer asks us to stop. Mirrors the old loop's "stop when not DOWNLOADPENDING".
static void dlThread(void* arg) {
    DlRing* r = (DlRing*)arg;
    while (!r->consumerStop) {
        u32 freeb = RING_SZ - ringUsed();
        if (freeb < TS_SZ) { svcSleepThread(2000000LL); continue; }   // full → wait 2ms
        u32 hi     = r->head & RING_MASK;
        u32 contig = RING_SZ - hi;                  // contiguous run to end of buffer
        u32 want   = freeb < contig ? freeb : contig;
        if (want > DL_CHUNK) want = DL_CHUNK;        // cap per download call
        u32 got = 0;
        Result dl = httpcDownloadData(r->ctx, r->data + hi, want, &got);
        if (got > 0) {
            LightLock_Lock(&r->lock);
            r->head += got;
            LightLock_Unlock(&r->lock);
        }
        if (dl != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;    // stream finished
    }
    r->producerDone = true;
}

// Consumer side: copy exactly n bytes out of the ring (handling wrap). The caller
// must have already confirmed ringUsed() >= n.
static void ringTake(u8* out, u32 n) {
    u32 ti     = g_ring.tail & RING_MASK;
    u32 contig = RING_SZ - ti;
    if (contig >= n) {
        memcpy(out, g_ring.data + ti, n);
    } else {
        memcpy(out, g_ring.data + ti, contig);
        memcpy(out + contig, g_ring.data, n - contig);
    }
    LightLock_Lock(&g_ring.lock);
    g_ring.tail += n;
    LightLock_Unlock(&g_ring.lock);
}

// Sentinel marker positions: 4 corners of the BGR565 output buffer, using the
// CURRENT coded dimensions (g_decW/g_decH) so they match where MVD writes.
// MVD overwrites these when it actually decodes a frame; if they survive a
// process/render call, no frame was produced. (0x11 per reference impl.)
static inline void mvdSetSentinels(u8* buf) {
    u32 sz = g_decW * g_decH * 2;
    buf[0]              = 0x11;
    buf[g_decW*2 - 1]   = 0x11;
    buf[sz - g_decW*2]  = 0x11;
    buf[sz - 1]         = 0x11;
}
static inline bool mvdSentinelsChanged(const u8* buf) {
    u32 sz = g_decW * g_decH * 2;
    return buf[0]              != 0x11 ||
           buf[g_decW*2 - 1]   != 0x11 ||
           buf[sz - g_decW*2]  != 0x11 ||
           buf[sz - 1]         != 0x11;
}

// ─── Minimal H.264 SPS parser (coded resolution only) ────────────────────────
struct BitReader { const u8* d; u32 nbits; u32 pos; };
static u32 brU1(BitReader* b) {
    if (b->pos >= b->nbits) return 0;
    u32 v = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1; b->pos++; return v;
}
static u32 brUn(BitReader* b, int n) { u32 v = 0; while (n-- > 0) v = (v << 1) | brU1(b); return v; }
static u32 brUE(BitReader* b) {
    int z = 0; while (b->pos < b->nbits && brU1(b) == 0 && z < 31) z++;
    return ((1u << z) - 1) + brUn(b, z);
}
static int brSE(BitReader* b) { u32 k = brUE(b); return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1); }

// Parse coded width/height from an SPS NAL (payload starts at the NAL header byte).
static bool parseSPS(const u8* nal, u32 len, u32* outW, u32* outH) {
    u8 rbsp[96]; u32 r = 0;
    for (u32 i = 1; i < len && r < sizeof(rbsp); i++) {     // skip NAL header byte
        if (i >= 2 && nal[i] == 3 && nal[i-1] == 0 && nal[i-2] == 0) continue; // emu prevention
        rbsp[r++] = nal[i];
    }
    BitReader b = { rbsp, r * 8, 0 };
    u32 profile = brUn(&b, 8); brUn(&b, 8); brUn(&b, 8);    // profile / constraints / level
    brUE(&b);                                                // seq_parameter_set_id
    if (profile==100||profile==110||profile==122||profile==244||profile==44||
        profile==83||profile==86||profile==118||profile==128||profile==138||
        profile==139||profile==134||profile==135) {
        u32 chroma = brUE(&b);
        if (chroma == 3) brU1(&b);
        brUE(&b); brUE(&b); brU1(&b);                        // bit depths + qpprime
        if (brU1(&b)) {                                      // scaling matrix present
            int cnt = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < cnt; i++)
                if (brU1(&b)) {                              // list present → skip it
                    int sz = (i < 6) ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < sz; j++) {
                        if (next != 0) { int d = brSE(&b); next = (last + d + 256) & 255; }
                        last = (next == 0) ? last : next;
                    }
                }
        }
    }
    brUE(&b);                                                // log2_max_frame_num
    u32 poc = brUE(&b);
    if (poc == 0) brUE(&b);
    else if (poc == 1) {
        brU1(&b); brSE(&b); brSE(&b);
        u32 n = brUE(&b); for (u32 i = 0; i < n; i++) brSE(&b);
    }
    brUE(&b); brU1(&b);                                      // max_num_ref_frames + gaps flag
    u32 wMbs = brUE(&b), hMaps = brUE(&b);
    u32 frameMbsOnly = brU1(&b);
    u32 w = (wMbs + 1) * 16;
    u32 h = (hMaps + 1) * 16 * (2 - frameMbsOnly);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return false;
    *outW = w; *outH = h; return true;
}

// ─── Access-unit H.264 decoder ───────────────────────────────────────────────
// Faithful adaptation of Core-2-Extreme/Video_player_for_3DS Util_decoder_mvd_decode,
// for an Annex-B MPEG-TS source instead of FFmpeg/AVCC:
//   1. Rebuild the access unit dropping AUD(9)/filler(12) NALs that MVD chokes on.
//   2. Re-arm the output buffer with MVDSTD_SetConfig() *before every frame*.
//   3. Feed the whole access unit in one mvdstdProcessVideoFrame() call.
//      The first AU is fed twice: pass 1 registers SPS/PPS, pass 2 decodes the IDR.
//   4. The frame is usually written during process(); check sentinels first and
//      only fall back to polling mvdstdRenderVideoFrame() if nothing appeared.
static void processH264(u8* pes, u32 pesLen,
                        u8* feedBuf, u8* mvdOut,
                        MVDSTD_Config* cfg, bool* first,
                        bool* stop, u32* frameCount, FILE* dbg, long long auPts) {
    (void)first;
    static u32 auCount = 0;
    auCount++;
    bool log = (auCount <= 25 || *frameCount < 5);
    u32 bufSz = g_decW * g_decH * 2;

    // Feed ONE NAL per mvdstdProcessVideoFrame call (the reference never lumps
    // SPS/PPS/slice together): SPS(7)/PPS(8)→PARAMSET, VCL slice(1..5)→FRAMEREADY→render.
    u32 pos = 0;
    while (pos + 3 < pesLen && !*stop) {
        bool sc3 = pes[pos]==0 && pes[pos+1]==0 && pes[pos+2]==1;
        bool sc4 = !sc3 && pes[pos]==0 && pes[pos+1]==0 &&
                   pes[pos+2]==0 && pes[pos+3]==1;
        if (!sc3 && !sc4) { pos++; continue; }

        u32 scLen   = sc4 ? 4 : 3;
        u32 payload = pos + scLen;           // first byte after start code
        u32 nalEnd  = pesLen;
        for (u32 s = payload + 1; s + 2 < pesLen; s++) {
            if (pes[s]==0 && pes[s+1]==0 &&
                (pes[s+2]==1 || (s+3<pesLen && pes[s+2]==0 && pes[s+3]==1))) {
                nalEnd = s; break;
            }
        }
        u8  nalType  = pes[payload] & 0x1F;
        u32 nalBytes = nalEnd - payload;     // NAL payload (no start code)
        pos = nalEnd;

        if (nalBytes == 0 || nalType == 9 || nalType == 12) continue;  // skip AUD/filler
        if (3 + nalBytes > NAL_SZ) continue;

        // SPS: parse the real coded resolution and reconfigure MVD if it changed.
        // MVD render() writes nothing unless the config dims match the decoded frame.
        if (nalType == 7) {
            u32 w, h;
            if (parseSPS(pes + payload, nalBytes, &w, &h) &&
                (w != g_decW || h != g_decH)) {
                g_decW = w; g_decH = h;
                bufSz  = g_decW * g_decH * 2;
                mvdstdGenerateDefaultConfig(cfg, g_decW, g_decH, g_decW, g_decH,
                                            nullptr, nullptr, nullptr);
                cfg->physaddr_outdata0 = osConvertVirtToPhys(mvdOut);
                cfg->physaddr_outdata1 = osConvertVirtToPhys(mvdOut);
                DLOG(dbg, "SPS dims=%lux%lu (reconfigured MVD)\n",
                     (unsigned long)g_decW, (unsigned long)g_decH);
            }
        }

        // Build a single Annex-B NAL (00 00 01 + payload) in linear feedBuf
        feedBuf[0]=0; feedBuf[1]=0; feedBuf[2]=1;
        memcpy(feedBuf + 3, pes + payload, nalBytes);
        u32 feedLen = 3 + nalBytes;
        GSPGPU_FlushDataCache(feedBuf, feedLen);

        bool isVCL = (nalType >= 1 && nalType <= 5);

        if (isVCL) {                         // arm output buffer just before a frame NAL
            mvdSetSentinels(mvdOut);
            GSPGPU_FlushDataCache(mvdOut, bufSz);
            MVDSTD_SetConfig(cfg);
        }

        Result rp = mvdstdProcessVideoFrame(feedBuf, feedLen, 0, nullptr);
        if (rp == (Result)MVD_STATUS_INCOMPLETEPROCESSING)   // retry once
            rp = mvdstdProcessVideoFrame(feedBuf, feedLen, 0, nullptr);

        if (log) DLOG(dbg, "NAL t=%u sz=%u proc=%08lX\n",
                     (unsigned)nalType, (unsigned)nalBytes, (unsigned long)rp);

        if (!isVCL) continue;                // parameter sets / SEI: no frame to render

        // Render the decoded frame into mvdOut (NULL = no re-SetConfig, patched mvd.c)
        bool got = false;
        Result rr = (Result)MVD_STATUS_BUSY;
        for (int i = 0; i < 256; i++) {
            rr = mvdstdRenderVideoFrame(nullptr, false);
            GSPGPU_InvalidateDataCache(mvdOut, bufSz);
            if (mvdSentinelsChanged(mvdOut)) { got = true; break; }
            if (rr != (Result)MVD_STATUS_BUSY) break;
        }
        if (log) DLOG(dbg, " rend=%08lX got=%d px=%02X%02X\n",
                     (unsigned long)rr, (int)got, mvdOut[0], mvdOut[1]);

        if (got) {
            paceToPts(auPts);            // sleep until this frame's presentation time
            blitFrame(mvdOut, *frameCount);
            (*frameCount)++;
            if (*frameCount <= 5) DLOG(dbg, "frame %u\n", (unsigned)*frameCount);
        }
        hidScanInput();
        if (hidKeysDown() & KEY_B) { *stop = true; return; }
    }
}

// ─── Player entry point ───────────────────────────────────────────────────────
bool playerPlay(const std::string& url, long long runTimeTicks,
                const std::string& series, const std::string& title, int year) {
    // C2D_CreateScreenTarget replaced gfx's framebuffer pointers with its own VRAM
    // allocation. After C3D_Fini that VRAM is freed but the pointers stay stale.
    // gfxSetScreenFormat is a no-op when the format hasn't changed, so it doesn't
    // fix the pointers. Full gfxExit+gfxInitDefault gives us fresh linear-memory
    // framebuffers that gfxGetFramebuffer and gfxSwapBuffers can safely use.
    gfxExit();
    gfxInitDefault();

    // Clear the top screen before playback (both buffers). When debug is on it
    // paints solid green as a framebuffer-path test; otherwise plain black.
    {
        u8 gch = g_dbg ? 255 : 0;  // green channel
        for (int pass = 0; pass < 2; pass++) {
            u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
            for (u32 i = 0; i < FB_W * FB_H * 3; i += 3) {
                fb[i]   = 0; fb[i+1] = gch; fb[i+2] = 0;
            }
            GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
            gspWaitForVBlank();
            gfxScreenSwapBuffers(GFX_TOP, false);
        }
    }

    // Bottom-screen console (must come after gfxInitDefault). Stays blank unless
    // debug is toggled on with X + D-Pad Up.
    consoleInit(GFX_BOTTOM, NULL);
    DBG("playerPlay\n");

    FILE* dbg = fopen("/3ds/3dsfin/player_debug.txt", "w");
    if (dbg) {
        fprintf(dbg, "BUILD=sps-dims-3\n");
        fprintf(dbg, "URL: %s\n\n", url.c_str());
        fflush(dbg);
    }

    // Linear memory buffers (g_ring.data is the background download ring)
    g_ring.data = (u8*)linearAlloc(RING_SZ);
    u8* pesBuf  = (u8*)linearAlloc(PES_SZ);
    u8* nalBuf  = (u8*)linearAlloc(NAL_SZ);
    u8* mvdOut  = (u8*)linearAlloc(VID_W * VID_H * 2);

    if (!g_ring.data || !pesBuf || !nalBuf || !mvdOut) {
        printf("alloc failed\n");
        if (dbg) { fprintf(dbg, "alloc failed\n"); fclose(dbg); }
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(mvdOut);
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("Buffers OK\n");

    // Reset coded dims to the max so the first SPS always triggers reconfigure.
    g_decW = VID_W; g_decH = VID_H;

    // Reset the frame-pacing anchor (set on the first displayed frame below).
    g_pacePts0 = -1; g_paceWall0 = 0;

    // Init MVD hardware decoder
    Result mvdRet = mvdstdInit(MVDMODE_VIDEOPROCESSING,
                               MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                               MVD_DEFAULT_WORKBUF_SIZE, nullptr);
    if (R_FAILED(mvdRet)) {
        printf("mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet);
        if (dbg) { fprintf(dbg, "mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet); fclose(dbg); }
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(mvdOut);
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("MVD OK\n");

    MVDSTD_Config mvdCfg;
    mvdstdGenerateDefaultConfig(&mvdCfg, VID_W, VID_H, VID_W, VID_H,
                                nullptr, nullptr, nullptr);
    mvdCfg.physaddr_outdata0 = osConvertVirtToPhys(mvdOut);
    mvdCfg.physaddr_outdata1 = osConvertVirtToPhys(mvdOut);
    MVDSTD_SetConfig(&mvdCfg);
    DLOG(dbg, "mvdOut virt=%08lX phys=%08lX\n",
         (unsigned long)mvdOut,
         (unsigned long)mvdCfg.physaddr_outdata0);

    // Override probe: stock libctru returns -1 (FFFFFFFF) for a NULL config;
    // our vendored/patched mvd.c allows NULL. This single line proves which
    // mvd.c is actually linked into the running binary.
    Result nullProbe = mvdstdRenderVideoFrame(nullptr, false);
    DLOG(dbg, "NULLrender probe=%08lX (FFFFFFFF=stock libctru, else=patched mvd.c)\n",
         (unsigned long)nullProbe);

    // Open HTTP stream
    httpcContext ctx;
    bool    ok         = false;
    u32     httpStatus = 0;
    DBG("HTTP open...\n");
    if (R_SUCCEEDED(httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1))) {
        httpcAddRequestHeaderField(&ctx, "User-Agent", "3DSFin/0.1");
        if (R_SUCCEEDED(httpcBeginRequest(&ctx))) {
            httpcGetResponseStatusCode(&ctx, &httpStatus);
            ok = (httpStatus >= 200 && httpStatus < 300);
        }
        if (!ok) httpcCloseContext(&ctx);
    }
    DBG("HTTP: %u\n", (unsigned)httpStatus);
    if (dbg) { fprintf(dbg, "HTTP status: %u\n", (unsigned)httpStatus); fflush(dbg); }

    if (!ok) {
        printf("HTTP fail, B to exit\n");
        if (dbg) fclose(dbg);
        mvdstdExit();
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(mvdOut);
        // Wait for B so user can read the error
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_B) break;
        }
        return false;
    }

    DBG("Streaming... B=stop\n");

    // ─── Decode loop ─────────────────────────────────────────────────────────
    int  pmtPid    = -1;
    int  vidPid    = -1;
    u32  pesLen    = 0;
    bool pesActive = false;
    bool stop       = false;
    bool mvdFirst   = true;   // first access unit is fed twice (params, then decode)
    u32  frameCount = 0;
    u32  pktCount   = 0;

    bool dbgComboPrev = false;   // edge-detect for the X + D-Pad Up debug toggle

    // Seek-bar state: position from PES PTS, total from the Jellyfin item.
    double    durSec      = runTimeTicks > 0 ? runTimeTicks / 10000000.0 : 0.0;
    double    posSec      = 0.0;
    long long firstPts    = -1;          // PTS of the first frame (position origin)
    long long curPesPts   = -1;          // PTS of the access unit now being accumulated
    int       lastShownSec = -1;         // throttle: redraw bar only when seconds change

    if (!g_dbg) drawMeta(series, title, year);   // static metadata above the seek bar

    // Start the background download thread filling the ring. Same priority as the
    // main thread so the two round-robin on the core; the main thread yields often
    // (pacing sleeps, vblank waits), letting the producer keep the ring topped up.
    g_ring.ctx          = &ctx;
    g_ring.head         = 0;
    g_ring.tail         = 0;
    g_ring.producerDone = false;
    g_ring.consumerStop = false;
    LightLock_Init(&g_ring.lock);
    s32 mainPrio = 0x30;
    svcGetThreadPriority(&mainPrio, CUR_THREAD_HANDLE);
    // core 0 (same as main): equal-priority round-robin, and a shared L1 so the
    // ring memory is trivially coherent between producer and consumer.
    Thread dlThr = threadCreate(dlThread, &g_ring, 32 * 1024, mainPrio, 0, false);
    if (dbg) { fprintf(dbg, "dlThread=%p prio=%ld\n", (void*)dlThr, (long)mainPrio); fflush(dbg); }

    // "Buffering…" hint at the top of the console so a stall reads as buffering
    // rather than a crash. rebuf tracks whether the hint is currently shown.
    bool rebuf = true;
    if (!g_dbg) printf("\x1b[1;5HBuffering...   ");

    // Prebuffer: wait until the ring holds PREBUF_SZ (or the stream ended) so a
    // brief network dip after playback starts doesn't immediately underrun.
    while (!stop && ringUsed() < PREBUF_SZ && !g_ring.producerDone) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) { stop = true; break; }
        svcSleepThread(10000000LL);   // 10ms
    }

    // A single reusable TS packet scratch (the ring hands out whole packets).
    u8 pkt[TS_SZ];

    while (!stop) {
        hidScanInput();

        // Toggle the on-screen debug overlay when X + D-Pad Up are held together.
        bool dbgCombo = (hidKeysHeld() & KEY_X) && (hidKeysHeld() & KEY_DUP);
        if (dbgCombo && !dbgComboPrev) {
            g_dbg = !g_dbg;
            if (g_dbg) printf("[debug ON]\n");
            else {                                     // back to clean view: redraw all
                consoleClear();
                drawMeta(series, title, year);
                lastShownSec = -1;                     // force seek-bar redraw
            }
        }
        dbgComboPrev = dbgCombo;

        if (hidKeysDown() & KEY_B) break;

        // Drain whole TS packets out of the ring. Decoding a frame paces+blits
        // inside processH264 (it may sleep); meanwhile dlThread keeps refilling the
        // ring. Cap the batch so the debug toggle and seek bar stay responsive.
        int processed = 0;
        while (!stop && ringUsed() >= TS_SZ && processed < 128) {
            ringTake(pkt, TS_SZ);
            processed++;
            pktCount++;
            if (pktCount % 500 == 0) {
                DBG("pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
                if (dbg) {
                    fprintf(dbg,"pkts=%u pmtPid=%d vidPid=%d frames=%u used=%u\n",
                            (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount,
                            (unsigned)ringUsed());
                    fflush(dbg);
                }
            }

            if (pkt[0] != 0x47) continue;
            int pid = tsPid(pkt);
            int psz = 0;
            const u8* pay = tsPayload(pkt, &psz);
            bool pusi = tsPUSI(pkt);

            if (pid==0 && pay && pmtPid==-1) {
                pmtPid = parsePAT(pay, psz);
                if (pmtPid!=-1) {
                    DBG("PAT->pmtPid=%d\n", pmtPid);
                    if (dbg) { fprintf(dbg,"PAT pmtPid=%d\n",pmtPid); fflush(dbg); }
                }
            } else if (pmtPid!=-1 && pid==pmtPid && pay && vidPid==-1) {
                vidPid = parsePMT(pay, psz);
                if (vidPid!=-1) {
                    DBG("PMT->vidPid=%d\n", vidPid);
                    if (dbg) { fprintf(dbg,"PMT vidPid=%d\n",vidPid); fflush(dbg); }
                }
            } else if (vidPid!=-1 && pid==vidPid && pay) {
                if (pusi) {
                    long long pts = pesPTS(pay, psz);
                    if (pts >= 0) {
                        if (firstPts < 0) firstPts = pts;
                        long long d = pts - firstPts;
                        if (d < 0) d += (1LL << 33);   // 33-bit PTS wraparound
                        posSec = d / 90000.0;
                    }
                    if (pesActive && pesLen > 0)
                        processH264(pesBuf,pesLen,nalBuf,mvdOut,&mvdCfg,&mvdFirst,&stop,&frameCount,dbg,curPesPts);
                    curPesPts = pts;   // PTS now belongs to the AU starting here
                    int skip = pesHeaderLen(pay,psz);
                    pesLen = 0; pesActive = true;
                    int cp = psz-skip;
                    if (cp>0 && (u32)cp<=PES_SZ) { memcpy(pesBuf,pay+skip,cp); pesLen=cp; }
                } else if (pesActive && pesLen+psz<=PES_SZ) {
                    memcpy(pesBuf+pesLen,pay,psz); pesLen+=psz;
                }
            }
        }

        // Buffering indicator: clear once frames flow again, re-show on underrun.
        if (!g_dbg) {
            if (processed > 0) {
                if (rebuf) { printf("\x1b[1;5H               "); rebuf = false; }
            } else if (!g_ring.producerDone && !rebuf) {
                printf("\x1b[1;5HBuffering...   "); rebuf = true;
            }
        }

        // Refresh the seek bar once per second of playback (skip while the debug
        // console is showing, so the two don't fight over the bottom screen).
        if (!g_dbg && (int)posSec != lastShownSec) {
            drawSeekBar(posSec, durSec);
            lastShownSec = (int)posSec;
        }

        // Stream finished and fully drained → done.
        if (g_ring.producerDone && ringUsed() < TS_SZ) break;
        // Nothing to do this pass (waiting on the network) → yield briefly.
        if (processed == 0) svcSleepThread(5000000LL);   // 5ms
    }

    DBG("End: pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
    if (dbg) {
        fprintf(dbg,"End: pkts=%u pmtPid=%d vidPid=%d frames=%u\n",
                (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount);
        fclose(dbg);
    }

    svcSleepThread(2000000000LL); // show stats for 2s before returning

    // Stop the producer: ask it to quit, then cancel any in-flight download so a
    // blocking httpcDownloadData returns and the thread can exit, then join it.
    g_ring.consumerStop = true;
    httpcCancelConnection(&ctx);
    threadJoin(dlThr, 5000000000LL);
    threadFree(dlThr);

    httpcCloseContext(&ctx);
    mvdstdExit();
    linearFree(g_ring.data); linearFree(pesBuf);
    linearFree(nalBuf); linearFree(mvdOut);
    return true;
}
