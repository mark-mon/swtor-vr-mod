// Minimal matrix math for D3D-style row-vector matrices (v' = v * M), row-major storage m[row][col].
#pragma once
#include <cmath>
#include <cstring>

namespace svr {

struct Mat4 {
    float m[4][4];
    static Mat4 Identity() { Mat4 r = {}; r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r; }
    static Mat4 From(const void* p) { Mat4 r; memcpy(r.m, p, 64); return r; }
    void To(void* p) const { memcpy(p, m, 64); }
};

inline Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Inverse of a rigid transform (rotation rows 0..2 orthonormal, translation in row 3).
inline Mat4 InverseRigid(const Mat4& w) {
    Mat4 r = Mat4::Identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = w.m[j][i];
    // t' = -t * R^T  ->  t'_j = -sum_k t_k * R[j][k]
    for (int j = 0; j < 3; j++) r.m[3][j] = -(w.m[3][0] * w.m[j][0] + w.m[3][1] * w.m[j][1] + w.m[3][2] * w.m[j][2]);
    return r;
}

// General 4x4 inverse (cofactor expansion).
inline bool Inverse(const Mat4& a, Mat4& out) {
    const float* m = &a.m[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) (&out.m[0][0])[i] = inv[i] * det;
    return true;
}

inline Mat4 RotY(float rad) {
    Mat4 r = Mat4::Identity();
    float c = cosf(rad), s = sinf(rad);
    r.m[0][0] = c; r.m[0][2] = -s; r.m[2][0] = s; r.m[2][2] = c;
    return r;
}
inline Mat4 RotX(float rad) {
    Mat4 r = Mat4::Identity();
    float c = cosf(rad), s = sinf(rad);
    r.m[1][1] = c; r.m[1][2] = s; r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}
inline Mat4 Translate(float x, float y, float z) { Mat4 r = Mat4::Identity(); r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z; return r; }

// Quaternion (x,y,z,w) -> rotation matrix (row-vector form).
inline Mat4 FromQuat(float x, float y, float z, float w) {
    Mat4 r = Mat4::Identity();
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y + z * w);     r.m[0][2] = 2 * (x * z - y * w);
    r.m[1][0] = 2 * (x * y - z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z + x * w);
    r.m[2][0] = 2 * (x * z + y * w);     r.m[2][1] = 2 * (y * z - x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

} // namespace svr
