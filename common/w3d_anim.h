// w3d_anim.h — skeleton pose of a CMDL creature model at a given time
// (shared by the 3DS client, the converter's preview and the tests).
//
// Bone matrices are 3x4 row-major (rows = x, y, z), mapping bind-pose model
// space to animated model space, exactly like WoWee computeBoneMatrices():
//   M_bone = M_parent * T(pivot) * T(t) * R(q) * S(s) * T(-pivot)
// Samples are linearly interpolated (rotations normalised-lerped), which is
// what WoWee does between keys (it slerps; at W3D_ANIM_FPS the difference is
// far below a pixel).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "w3d_format.h"

typedef struct {
    float m[12];
} W3dMat34;

static inline const W3dCreatureHeader* w3d_creature(const uint8_t* blob) { return (const W3dCreatureHeader*)blob; }

// Sequence index with this animation id, or -1.
static inline int w3d_find_seq(const uint8_t* blob, uint16_t animId) {
    const W3dCreatureHeader* h = w3d_creature(blob);
    const W3dAnimSeq* s = (const W3dAnimSeq*)(blob + h->offSeqs);
    for (uint32_t i = 0; i < h->seqCount; ++i)
        if (s[i].animId == animId) return (int)i;
    return -1;
}

// 6.36: does the sequence move anything? (more than one frame, and at least one bone channel with keys over time) - a still pose
// is a model that stands like a statue however long it "plays" (diagnostics: perf_events.txt "ANIM MODEL")
static inline int w3d_seq_animated(const uint8_t* blob, int seq) {
    const W3dCreatureHeader* h = w3d_creature(blob);
    if (seq < 0 || (uint32_t)seq >= h->seqCount) return 0;
    const W3dAnimSeq* sq = (const W3dAnimSeq*)(blob + h->offSeqs) + seq;
    if (sq->frames < 2 || sq->durationMs == 0) return 0;
    const uint32_t* ch = (const uint32_t*)(blob + sq->offChannels);
    for (uint32_t b = 0; b < h->boneCount * 3u; ++b)
        if (ch[b] != W3D_CHAN_NONE && (ch[b] & W3D_CHAN_ANIM)) return 1;
    return 0;
}

static inline void w3d_quat_to_mat(float x, float y, float z, float w, float r[9]) {
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    r[0] = 1 - 2 * (yy + zz); r[1] = 2 * (xy - wz);     r[2] = 2 * (xz + wy);
    r[3] = 2 * (xy + wz);     r[4] = 1 - 2 * (xx + zz); r[5] = 2 * (yz - wx);
    r[6] = 2 * (xz - wy);     r[7] = 2 * (yz + wx);     r[8] = 1 - 2 * (xx + yy);
}

// Pose of every bone (out[boneCount]). seq < 0 = bind pose. loop: wrap time,
// else clamp to the last frame (death).
static inline void w3d_anim_pose(const uint8_t* blob, int seq, float timeMs, int loop, W3dMat34* out) {
    const W3dCreatureHeader* h = w3d_creature(blob);
    const W3dBone* bones = (const W3dBone*)(blob + h->offBones);
    const int16_t* keys = (const int16_t*)(blob + h->offKeys);
    const W3dAnimSeq* sq = (seq >= 0 && (uint32_t)seq < h->seqCount) ? (const W3dAnimSeq*)(blob + h->offSeqs) + seq : 0;
    const uint32_t* ch = sq ? (const uint32_t*)(blob + sq->offChannels) : 0;
    int i0 = 0, i1 = 0;
    float fr = 0;
    if (sq && sq->frames > 1 && sq->durationMs > 0) {
        float dur = (float)sq->durationMs;
        float t = timeMs;
        if (loop) { t = fmodf(t, dur); if (t < 0) t += dur; }
        else if (t > dur) t = dur;
        if (t < 0) t = 0;
        float pos = t / dur * (float)(sq->frames - 1);
        i0 = (int)pos;
        if (i0 >= sq->frames - 1) { i0 = sq->frames - 1; i1 = i0; fr = 0; }
        else { i1 = i0 + 1; fr = pos - (float)i0; }
    }
    for (uint32_t b = 0; b < h->boneCount; ++b) {
        float q[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0}, s[3] = {1, 1, 1};
        if (ch) {
            uint32_t c = ch[b * 3 + 0];
            if (c != W3D_CHAN_NONE) {
                const int16_t* k = keys + (c & ~W3D_CHAN_ANIM);
                if (c & W3D_CHAN_ANIM) {
                    const int16_t *a = k + i0 * 4, *bq = k + i1 * 4;
                    float d = (float)a[0] * bq[0] + (float)a[1] * bq[1] + (float)a[2] * bq[2] + (float)a[3] * bq[3];
                    float sgn = d < 0 ? -1.0f : 1.0f;
                    for (int j = 0; j < 4; ++j) q[j] = (a[j] + (sgn * bq[j] - a[j]) * fr) * (1.0f / 32767.0f);
                } else {
                    for (int j = 0; j < 4; ++j) q[j] = k[j] * (1.0f / 32767.0f);
                }
                float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (l > 1e-6f) for (int j = 0; j < 4; ++j) q[j] /= l;
                else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
            }
            c = ch[b * 3 + 1];
            if (c != W3D_CHAN_NONE) {
                const int16_t* k = keys + (c & ~W3D_CHAN_ANIM);
                if (c & W3D_CHAN_ANIM)
                    for (int j = 0; j < 3; ++j) t[j] = (k[i0 * 3 + j] + (k[i1 * 3 + j] - k[i0 * 3 + j]) * fr) * h->transScale;
                else
                    for (int j = 0; j < 3; ++j) t[j] = k[j] * h->transScale;
            }
            c = ch[b * 3 + 2];
            if (c != W3D_CHAN_NONE) {
                const int16_t* k = keys + (c & ~W3D_CHAN_ANIM);
                if (c & W3D_CHAN_ANIM)
                    for (int j = 0; j < 3; ++j) s[j] = (k[i0 * 3 + j] + (k[i1 * 3 + j] - k[i0 * 3 + j]) * fr) * W3D_SCALE_KEY;
                else
                    for (int j = 0; j < 3; ++j) s[j] = k[j] * W3D_SCALE_KEY;
                for (int j = 0; j < 3; ++j) if (s[j] < 0.001f) s[j] = 1.0f;   // as WoWee
            }
        }
        float r[9];
        w3d_quat_to_mat(q[0], q[1], q[2], q[3], r);
        const float* p = bones[b].pivot;
        // local = [R*S | p + t - R*S*p]
        float L[12];
        for (int row = 0; row < 3; ++row) {
            float a = r[row * 3 + 0] * s[0], bb = r[row * 3 + 1] * s[1], cc = r[row * 3 + 2] * s[2];
            L[row * 4 + 0] = a; L[row * 4 + 1] = bb; L[row * 4 + 2] = cc;
            L[row * 4 + 3] = p[row] + t[row] - (a * p[0] + bb * p[1] + cc * p[2]);
        }
        int par = bones[b].parent;
        if (par >= 0 && (uint32_t)par < b) {
            const float* P = out[par].m;
            float* M = out[b].m;
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 4; ++col)
                    M[row * 4 + col] = P[row * 4 + 0] * L[0 * 4 + col] + P[row * 4 + 1] * L[1 * 4 + col] + P[row * 4 + 2] * L[2 * 4 + col];
                M[row * 4 + 3] += P[row * 4 + 3];
            }
        } else {
            memcpy(out[b].m, L, sizeof L);
        }
    }
}
