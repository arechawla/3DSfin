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
                        bool* stop, u32* frameCount, FILE* dbg) {
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
            blitFrame(mvdOut, *frameCount);
            (*frameCount)++;
            if (*frameCount <= 5) DLOG(dbg, "frame %u\n", (unsigned)*frameCount);
        }
        hidScanInput();
        if (hidKeysDown() & KEY_B) { *stop = true; return; }
    }
}

// ─── Player entry point ───────────────────────────────────────────────────────
bool playerPlay(const std::string& url) {
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

    // Linear memory buffers
    u8* rdBuf  = (u8*)linearAlloc(RD_SZ);
    u8* pesBuf = (u8*)linearAlloc(PES_SZ);
    u8* nalBuf = (u8*)linearAlloc(NAL_SZ);
    u8* mvdOut = (u8*)linearAlloc(VID_W * VID_H * 2);

    if (!rdBuf || !pesBuf || !nalBuf || !mvdOut) {
        printf("alloc failed\n");
        if (dbg) { fprintf(dbg, "alloc failed\n"); fclose(dbg); }
        linearFree(rdBuf); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(mvdOut);
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("Buffers OK\n");

    // Reset coded dims to the max so the first SPS always triggers reconfigure.
    g_decW = VID_W; g_decH = VID_H;

    // Init MVD hardware decoder
    Result mvdRet = mvdstdInit(MVDMODE_VIDEOPROCESSING,
                               MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                               MVD_DEFAULT_WORKBUF_SIZE, nullptr);
    if (R_FAILED(mvdRet)) {
        printf("mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet);
        if (dbg) { fprintf(dbg, "mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet); fclose(dbg); }
        linearFree(rdBuf); linearFree(pesBuf);
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
        linearFree(rdBuf); linearFree(pesBuf);
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

    u8  partial[TS_SZ];
    int partialLen = 0;
    bool dbgComboPrev = false;   // edge-detect for the X + D-Pad Up debug toggle

    while (!stop) {
        hidScanInput();

        // Toggle the on-screen debug overlay when X + D-Pad Up are held together.
        bool dbgCombo = (hidKeysHeld() & KEY_X) && (hidKeysHeld() & KEY_DUP);
        if (dbgCombo && !dbgComboPrev) {
            g_dbg = !g_dbg;
            if (g_dbg) printf("[debug ON]\n");
            else       consoleClear();   // wipe the bottom screen when hiding
        }
        dbgComboPrev = dbgCombo;

        if (hidKeysDown() & KEY_B) break;

        u32    got   = 0;
        Result dlret = httpcDownloadData(&ctx, rdBuf, RD_SZ, &got);

        u8*  cur       = rdBuf;
        u32  remaining = got;

        // Finish any partial packet carried from last read
        if (partialLen > 0 && got > 0) {
            int need = TS_SZ - partialLen;
            if ((int)remaining >= need) {
                memcpy(partial + partialLen, cur, need);
                cur += need; remaining -= need; partialLen = 0;
                if (partial[0] == 0x47) {
                    int pid = tsPid(partial);
                    int psz = 0;
                    const u8* pay = tsPayload(partial, &psz);
                    bool pusi = tsPUSI(partial);
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
                            if (pesActive && pesLen > 0)
                                processH264(pesBuf,pesLen,nalBuf,mvdOut,&mvdCfg,&mvdFirst,&stop,&frameCount,dbg);
                            int skip = pesHeaderLen(pay,psz);
                            pesLen = 0; pesActive = true;
                            int cp = psz-skip;
                            if (cp>0 && (u32)cp<=PES_SZ) { memcpy(pesBuf,pay+skip,cp); pesLen=cp; }
                        } else if (pesActive && pesLen+psz<=PES_SZ) {
                            memcpy(pesBuf+pesLen,pay,psz); pesLen+=psz;
                        }
                    }
                }
            } else {
                memcpy(partial+partialLen, cur, remaining);
                partialLen += remaining; remaining = 0;
            }
        }

        // Process full TS packets from the read buffer
        while (!stop && remaining >= TS_SZ) {
            pktCount++;
            // Progress update every 500 packets
            if (pktCount % 500 == 0) {
                DBG("pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
                if (dbg) {
                    fprintf(dbg,"pkts=%u pmtPid=%d vidPid=%d frames=%u\n",
                            (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount);
                    fflush(dbg);
                }
            }

            if (cur[0] == 0x47) {
                int pid = tsPid(cur);
                int psz = 0;
                const u8* pay = tsPayload(cur, &psz);
                bool pusi = tsPUSI(cur);

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
                        if (pesActive && pesLen > 0)
                            processH264(pesBuf,pesLen,nalBuf,mvdOut,&mvdCfg,&mvdFirst,&stop,&frameCount,dbg);
                        int skip = pesHeaderLen(pay,psz);
                        pesLen = 0; pesActive = true;
                        int cp = psz-skip;
                        if (cp>0 && (u32)cp<=PES_SZ) { memcpy(pesBuf,pay+skip,cp); pesLen=cp; }
                    } else if (pesActive && pesLen+psz<=PES_SZ) {
                        memcpy(pesBuf+pesLen,pay,psz); pesLen+=psz;
                    }
                }
            }
            cur += TS_SZ; remaining -= TS_SZ;
        }

        if (remaining > 0) {
            memcpy(partial, cur, remaining);
            partialLen = remaining;
        }

        if (dlret != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;
    }

    DBG("End: pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
    if (dbg) {
        fprintf(dbg,"End: pkts=%u pmtPid=%d vidPid=%d frames=%u\n",
                (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount);
        fclose(dbg);
    }

    svcSleepThread(2000000000LL); // show stats for 2s before returning

    httpcCancelConnection(&ctx);
    httpcCloseContext(&ctx);
    mvdstdExit();
    linearFree(rdBuf); linearFree(pesBuf);
    linearFree(nalBuf); linearFree(mvdOut);
    return true;
}
