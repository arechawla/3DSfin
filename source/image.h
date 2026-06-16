#pragma once
#include <citro2d.h>
#include <cstddef>

// Decode a JPEG/PNG held in memory into a C2D_Image backed by a freshly
// allocated C3D_Tex (+ Tex3DS_SubTexture). Returns false on failure.
// On success the image must be released with Image_free().
bool Image_loadFromMemory(const unsigned char* data, size_t len, C2D_Image* out);

// Releases the texture/subtexture created by Image_loadFromMemory and clears
// the C2D_Image. Safe to call on an all-zero (never-loaded) image.
void Image_free(C2D_Image* img);
