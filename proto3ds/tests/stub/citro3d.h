#pragma once
#include "3ds.h"
typedef enum { GPU_RGBA8, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4, GPU_LA8, GPU_HILO8, GPU_L8, GPU_A8, GPU_LA4, GPU_L4, GPU_A4, GPU_ETC1, GPU_ETC1A4 } GPU_TEXCOLOR;
typedef enum { GPU_TEX_2D = 0 } GPU_TEXTURE_MODE_PARAM;
typedef enum { GPU_TEXFACE_2D = 0 } GPU_TEXFACE;
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
typedef enum { GPU_CLAMP_TO_EDGE = 0, GPU_REPEAT = 2 } GPU_TEXTURE_WRAP_PARAM;
typedef struct { void* data; u16 width, height; GPU_TEXCOLOR fmt; int levels; u32 size; bool onVram; } C3D_Tex;
typedef struct { u16 width, height; u8 maxLevel; GPU_TEXCOLOR format; GPU_TEXTURE_MODE_PARAM type; bool onVram; } C3D_TexInitParams;
bool C3D_TexInitWithParams(C3D_Tex* t, void* cube, C3D_TexInitParams p);
bool C3D_TexInit(C3D_Tex* t, u16 w, u16 h, GPU_TEXCOLOR f);
void C3D_TexLoadImage(C3D_Tex* t, const void* data, GPU_TEXFACE face, int level);
void C3D_TexFlush(C3D_Tex* t);
void C3D_TexUpload(C3D_Tex* t, const void* data);
void C3D_TexSetFilter(C3D_Tex* t, GPU_TEXTURE_FILTER_PARAM a, GPU_TEXTURE_FILTER_PARAM b);
void C3D_TexSetFilterMipmap(C3D_Tex* t, GPU_TEXTURE_FILTER_PARAM a);
void C3D_TexSetWrap(C3D_Tex* t, GPU_TEXTURE_WRAP_PARAM s, GPU_TEXTURE_WRAP_PARAM tt);
void C3D_TexDelete(C3D_Tex* t);
typedef struct C3D_RenderTarget_tag C3D_RenderTarget;
void C3D_SyncTextureCopy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags);   // stage 6
int stubTextureCopies(void);
float C3D_GetDrawingTime(void); float C3D_GetProcessingTime(void); float C3D_GetCmdBufUsage(void);   // stage 6 (perf log)   // renderer is not part of the PC build
