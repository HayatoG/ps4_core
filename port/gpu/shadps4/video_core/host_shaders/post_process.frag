// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    // tlg: player color adjustments (tlg.ini color_adjust), on the display-encoded color.
    bool color_adjust;
    float brightness; // added, -0.5 to 0.5
    float contrast;   // around mid gray, 1 = unchanged
    float saturation; // 0 gray, 1 unchanged
    float user_gamma; // 1 unchanged, above 1 brighter midtones
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
        if (pp.color_adjust) {
            vec3 c = pow(clamp(color.rgb, 0.0, 1.0), vec3(1.0 / pp.user_gamma));
            c = (c + pp.brightness - 0.5) * pp.contrast + 0.5;
            c = mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, pp.saturation);
            color.rgb = clamp(c, 0.0, 1.0);
        }
    }
}
