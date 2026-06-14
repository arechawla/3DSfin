#include "player.h"
#include <3ds.h>
#include <cstring>
#include <cstdio>

// ─── Dimensions ──────────────────────────────────────────────────────────────
static constexpr u32 VID_W = 400, VID_H = 240;
static constexpr u32 FB_W  = 240, FB_H  = 400;

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
        printf("fb=%p px[0]=%04X px[1k]=%04X\n",
               (void*)fb, (unsigned)src[0], (unsigned)src[1000]);
    }

    for (u32 y = 0; y < VID_H; y++) {
        for (u32 x = 0; x < VID_W; x++) {
            u16 px  = src[y * VID_W + x];
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

// ─── NAL unit scanner ─────────────────────────────────────────────────────────
static void processH264(const u8* data, u32 len,
                        u8* nalBuf, u8* mvdOut,
                        MVDSTD_Config* cfg, bool* stop, u32* frameCount) {
    static u32  nalCount = 0;
    static bool inited   = false;
    if (!inited) {
        inited = true;
        // Print the actual values of MVD status constants so we know what to expect
        printf("FRAMEREADY=%08lX\n", (unsigned long)MVD_STATUS_FRAMEREADY);
        printf("PARAMSET  =%08lX\n", (unsigned long)MVD_STATUS_PARAMSET);
        // Sentinel-fill mvdOut: if pixels stay 0xAAAA after render, MVD never wrote here
        memset(mvdOut, 0xAA, VID_W * VID_H * 2);
        GSPGPU_FlushDataCache(mvdOut, VID_W * VID_H * 2);
        printf("sentinel filled\n");
    }

    u32 pos = 0;
    while (!*stop && pos + 3 < len) {
        bool sc3 = data[pos]==0 && data[pos+1]==0 && data[pos+2]==1;
        bool sc4 = !sc3 && pos+3 < len &&
                   data[pos]==0 && data[pos+1]==0 &&
                   data[pos+2]==0 && data[pos+3]==1;
        if (!sc3 && !sc4) { pos++; continue; }

        u32 nalStart = sc4 ? pos+1 : pos;
        u32 scanFrom = pos + (sc4 ? 4 : 3) + 1;

        u32 nalEnd = len;
        for (u32 s = scanFrom; s+2 < len; s++) {
            if (data[s]==0 && data[s+1]==0 &&
                (data[s+2]==1 || (s+3<len && data[s+2]==0 && data[s+3]==1))) {
                nalEnd = s; break;
            }
        }

        u32 nalSize = nalEnd - nalStart;
        if (nalSize > 0 && nalSize <= NAL_SZ) {
            memcpy(nalBuf, data + nalStart, nalSize);
            GSPGPU_FlushDataCache(nalBuf, nalSize);

            nalCount++;
            // NAL type sits in the first byte after the 3-byte start code (00 00 01)
            u8 nalType = (nalSize >= 4) ? (nalBuf[3] & 0x1F) : 0;
            bool log   = (nalCount <= 20);

            MVDSTD_ProcessNALUnitOut out;
            Result ret = mvdstdProcessVideoFrame(nalBuf, nalSize, 0, &out);
            if (log) printf("NAL#%u t=%u sz=%u ret=%08lX\n",
                            (unsigned)nalCount, (unsigned)nalType,
                            (unsigned)nalSize,  (unsigned long)ret);

            if (ret == MVD_STATUS_FRAMEREADY) {
                Result rr = mvdstdRenderVideoFrame(cfg, true);
                const u16* dp = (const u16*)mvdOut;
                u16 pre0  = dp[0];
                GSPGPU_InvalidateDataCache(mvdOut, VID_W * VID_H * 2);
                u16 post0 = dp[0];
                if (log || *frameCount < 3)
                    printf("  rend=%08lX pre=%04X post=%04X\n",
                           (unsigned long)rr, (unsigned)pre0, (unsigned)post0);
                blitFrame(mvdOut, *frameCount);
                (*frameCount)++;
                if (*frameCount <= 5) printf("frame %u\n", (unsigned)*frameCount);
                hidScanInput();
                if (hidKeysDown() & KEY_B) *stop = true;
            }
        }
        pos = nalEnd;
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

    // Bottom-screen debug console (must come after gfxInitDefault)
    consoleInit(GFX_BOTTOM, NULL);
    printf("playerPlay\n");

    FILE* dbg = fopen("/3ds/3dsfin/player_debug.txt", "w");
    if (dbg) { fprintf(dbg, "URL: %s\n\n", url.c_str()); fflush(dbg); }

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
    printf("Buffers OK\n");

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
    printf("MVD OK\n");

    MVDSTD_Config mvdCfg;
    mvdstdGenerateDefaultConfig(&mvdCfg, VID_W, VID_H, VID_W, VID_H,
                                nullptr, (u32*)mvdOut, (u32*)mvdOut);
    printf("mvdOut virt=%08lX\n",    (unsigned long)mvdOut);
    printf("       phys=%08lX\n",    (unsigned long)osConvertVirtToPhys(mvdOut));
    printf("cfg physaddr=%08lX\n",   (unsigned long)mvdCfg.physaddr_outdata0);

    // Open HTTP stream
    httpcContext ctx;
    bool    ok         = false;
    u32     httpStatus = 0;
    printf("HTTP open...\n");
    if (R_SUCCEEDED(httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1))) {
        httpcAddRequestHeaderField(&ctx, "User-Agent", "3DSFin/0.1");
        if (R_SUCCEEDED(httpcBeginRequest(&ctx))) {
            httpcGetResponseStatusCode(&ctx, &httpStatus);
            ok = (httpStatus >= 200 && httpStatus < 300);
        }
        if (!ok) httpcCloseContext(&ctx);
    }
    printf("HTTP: %u\n", (unsigned)httpStatus);
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

    printf("Streaming... B=stop\n");

    // ─── Decode loop ─────────────────────────────────────────────────────────
    int  pmtPid    = -1;
    int  vidPid    = -1;
    u32  pesLen    = 0;
    bool pesActive = false;
    bool stop      = false;
    u32  frameCount = 0;
    u32  pktCount   = 0;

    u8  partial[TS_SZ];
    int partialLen = 0;

    while (!stop) {
        hidScanInput();
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
                            printf("PAT->pmtPid=%d\n", pmtPid);
                            if (dbg) { fprintf(dbg,"PAT pmtPid=%d\n",pmtPid); fflush(dbg); }
                        }
                    } else if (pmtPid!=-1 && pid==pmtPid && pay && vidPid==-1) {
                        vidPid = parsePMT(pay, psz);
                        if (vidPid!=-1) {
                            printf("PMT->vidPid=%d\n", vidPid);
                            if (dbg) { fprintf(dbg,"PMT vidPid=%d\n",vidPid); fflush(dbg); }
                        }
                    } else if (vidPid!=-1 && pid==vidPid && pay) {
                        if (pusi) {
                            if (pesActive && pesLen > 0)
                                processH264(pesBuf,pesLen,nalBuf,mvdOut,&mvdCfg,&stop,&frameCount);
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
                printf("pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
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
                        printf("PAT->pmtPid=%d\n", pmtPid);
                        if (dbg) { fprintf(dbg,"PAT pmtPid=%d\n",pmtPid); fflush(dbg); }
                    }
                } else if (pmtPid!=-1 && pid==pmtPid && pay && vidPid==-1) {
                    vidPid = parsePMT(pay, psz);
                    if (vidPid!=-1) {
                        printf("PMT->vidPid=%d\n", vidPid);
                        if (dbg) { fprintf(dbg,"PMT vidPid=%d\n",vidPid); fflush(dbg); }
                    }
                } else if (vidPid!=-1 && pid==vidPid && pay) {
                    if (pusi) {
                        if (pesActive && pesLen > 0)
                            processH264(pesBuf,pesLen,nalBuf,mvdOut,&mvdCfg,&stop,&frameCount);
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

    printf("End: pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
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
