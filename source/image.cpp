#include "image.h"
#include <3ds.h>
#include <cstdlib>
#include <cstring>

// stb_image: JPEG (Jellyfin default) + PNG, decode-from-memory only.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD          // ARMv6 — no SSE; keep the portable path
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

static u32 nextPow2(u32 v) {
    if (v < 8) return 8;
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v++;
    return v;
}

bool Image_loadFromMemory(const unsigned char* data, size_t len, C2D_Image* out) {
    if (!data || len == 0 || !out) return false;

    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba) return false;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { stbi_image_free(rgba); return false; }

    u32 pw = nextPow2((u32)w), ph = nextPow2((u32)h);

    C3D_Tex* tex            = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    Tex3DS_SubTexture* sub  = (Tex3DS_SubTexture*)malloc(sizeof(Tex3DS_SubTexture));
    if (!tex || !sub) { free(tex); free(sub); stbi_image_free(rgba); return false; }

    if (!C3D_TexInit(tex, (u16)pw, (u16)ph, GPU_RGBA8)) {
        free(tex); free(sub); stbi_image_free(rgba); return false;
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    // Sub-rectangle: the real w*h sits in the top-left of the POT texture.
    sub->width  = (u16)w;
    sub->height = (u16)h;
    sub->left   = 0.0f;
    sub->top    = 1.0f;
    sub->right  = (float)w / (float)pw;
    sub->bottom = 1.0f - (float)h / (float)ph;

    // Stage in linear memory and byte-swap each texel so the GPU display
    // transfer tiles it into the texture in GPU_RGBA8 order. stb writes bytes
    // R,G,B,A (u32 = 0xAABBGGRR); bswap -> bytes A,B,G,R as the transfer wants.
    //
    // *** ON-DEVICE TUNABLES (if the art looks wrong): ***
    //   - colors with R/B swapped  -> remove the __builtin_bswap32 below
    //   - image upside-down        -> flip GX_TRANSFER_FLIP_VERT(1) to (0)
    u32* stage = (u32*)linearAlloc(pw * ph * 4);
    if (!stage) { C3D_TexDelete(tex); free(tex); free(sub); stbi_image_free(rgba); return false; }
    memset(stage, 0, pw * ph * 4);
    const u32* src = (const u32*)rgba;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            stage[y * pw + x] = __builtin_bswap32(src[y * w + x]);

    GSPGPU_FlushDataCache(stage, pw * ph * 4);
    C3D_SyncDisplayTransfer(
        stage,           GX_BUFFER_DIM(pw, ph),
        (u32*)tex->data, GX_BUFFER_DIM(pw, ph),
        GX_TRANSFER_FLIP_VERT(1)  | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)  |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    linearFree(stage);
    stbi_image_free(rgba);

    out->tex    = tex;
    out->subtex = sub;
    return true;
}

void Image_free(C2D_Image* img) {
    if (!img || !img->tex) return;
    C3D_TexDelete(img->tex);
    free(img->tex);
    free((void*)img->subtex);
    img->tex    = nullptr;
    img->subtex = nullptr;
}
