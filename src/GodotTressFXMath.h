#pragma once

#include "AMD_Types.h"
#include <cstring>
#include <cmath>
#include <cfloat>

// Minimal replacement for DirectXMath's XMMATRIX to avoid DirectX dependency.
// This is used by TressFX core (TressFXBoneSkinning.cpp).

struct XMFLOAT3 {
    float x, y, z;
};

struct XMVECTOR {
    float x, y, z, w;

    XMVECTOR operator+(const XMVECTOR& other) const {
        return { x + other.x, y + other.y, z + other.z, w + other.w };
    }

    XMVECTOR& operator+=(const XMVECTOR& other) {
        x += other.x;
        y += other.y;
        z += other.z;
        w += other.w;
        return *this;
    }

    XMVECTOR operator-(const XMVECTOR& other) const {
        return { x - other.x, y - other.y, z - other.z, w - other.w };
    }

    XMVECTOR operator*(float scalar) const {
        return { x * scalar, y * scalar, z * scalar, w * scalar };
    }

    XMVECTOR operator/(float scalar) const {
        float inv = 1.0f / scalar;
        return { x * inv, y * inv, z * inv, w * inv };
    }
};

struct XMMATRIX {
    union {
        AMD::float4x4 mat;
        float m[4][4];
        float v[16];
    };

    XMMATRIX() {
        std::memset(v, 0, sizeof(v));
    }

    // Constructor from AMD::float4x4
    XMMATRIX(const AMD::float4x4& other) {
        std::memcpy(v, other.m, sizeof(v));
    }

    // Operator * (scalar)
    XMMATRIX operator*(float scalar) const {
        XMMATRIX result;
        for (int i = 0; i < 16; ++i) {
            result.v[i] = v[i] * scalar;
        }
        return result;
    }

    // Operator += (matrix)
    XMMATRIX& operator+=(const XMMATRIX& other) {
        for (int i = 0; i < 16; ++i) {
            v[i] += other.v[i];
        }
        return *this;
    }

    // Operator * (matrix) - Standard matrix multiplication
    XMMATRIX operator*(const XMMATRIX& other) const {
        XMMATRIX result;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                result.m[r][c] = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    result.m[r][c] += m[r][k] * other.m[k][c];
                }
            }
        }
        return result;
    }
    
    // Operator /= (scalar) - Used in TressFXBoneSkinning.cpp
    XMMATRIX& operator/=(float scalar) {
        float invScalar = 1.0f / scalar;
        for (int i = 0; i < 16; ++i) {
            v[i] *= invScalar;
        }
        return *this;
    }
};

// Helper to convert to AMD::float4x4 if needed
inline AMD::float4x4 XMMATRIXToAMD(const XMMATRIX& x) {
    AMD::float4x4 res;
    std::memcpy(res.m, x.v, sizeof(res.m));
    return res;
}

inline XMVECTOR XMVector4Transform(const XMVECTOR& v, const XMMATRIX& m) {
    XMVECTOR res;
    // Assuming row-major or column-major? DirectXMath is row-major usually, but let's check TressFX usage.
    // TressFXBoneSkinning.cpp: pos = XMVector4Transform(pos, bone_matrix);
    // Usually v * M.
    res.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0];
    res.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1];
    res.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2];
    res.w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3];
    return res;
}

inline void XMStoreFloat3(XMFLOAT3* pDestination, XMVECTOR V) {
    pDestination->x = V.x;
    pDestination->y = V.y;
    pDestination->z = V.z;
}
