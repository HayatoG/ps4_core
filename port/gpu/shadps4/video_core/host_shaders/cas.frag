// SPDX-FileCopyrightText: Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
// tlg: AMD FidelityFX CAS (cas/ffx_cas.h, MIT) in sharpen-only mode, as a full-screen pixel
// shader (fs_tri.vert) like the presenter's other passes.

#version 450
#extension GL_GOOGLE_include_directive : require

layout (push_constant) uniform const_buffer {
    uvec4 Const0;
    uvec4 Const1;
};

#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"

layout (binding = 0) uniform sampler2D InputTexture;
layout (location = 0) out vec4 color;

AF3 CasLoad(ASU2 p) { return texelFetch(InputTexture, p, 0).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "cas/ffx_cas.h"

void main() {
    AF3 c;
    CasFilter(c.r, c.g, c.b, AU2(gl_FragCoord.xy), Const0, Const1, true);
    color = AF4(c, 1.0);
}
