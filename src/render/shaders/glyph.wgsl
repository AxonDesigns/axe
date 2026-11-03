// Instanced glyph quads sampling coverage from an R8 atlas page. Quads cover whole physical
// pixels and map 1:1 onto atlas texels, so coverage is read with textureLoad (no filtering,
// no bleeding, identical on every device). Blending happens in linear light (the target is
// sRGB); the contrast term thickens dark text, which linear blending would otherwise thin.
// See docs/text.md for the formula and comparisons.

struct Globals {
    viewport: vec2<f32>,
    contrast: f32,      // k in c + k c (1 - c) (1 - L)
    gamma: f32,         // coverage exponent 1/gamma; 1 = off
    origin: vec2<f32>,  // physical px of the target's top-left (a layer's bounds)
    _pad: vec2<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;
@group(1) @binding(0) var atlas: texture_2d<f32>;

struct Instance {
    @location(0) rect: vec4<f32>,   // x, y, width, height (whole physical pixels)
    @location(1) uv: vec4<f32>,     // atlas x, y (texels), reserved
    @location(2) color: vec4<f32>,  // linear, premultiplied
};

struct VsOut {
    @builtin(position) position: vec4<f32>,
    @location(0) texel: vec2<f32>,
    @location(1) color: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32, inst: Instance) -> VsOut {
    let corner = vec2<f32>(f32(vertex & 1u), f32(vertex >> 1u));
    let p = inst.rect.xy + corner * inst.rect.zw - globals.origin;
    var out: VsOut;
    out.position = vec4<f32>(p.x / globals.viewport.x * 2.0 - 1.0,
                             1.0 - p.y / globals.viewport.y * 2.0, 0.0, 1.0);
    out.texel = inst.uv.xy + corner * inst.rect.zw;
    out.color = inst.color;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Fragments sit at pixel centers, so floor() gives the texel this pixel maps to.
    var c = textureLoad(atlas, vec2<i32>(floor(in.texel)), 0).r;
    let straight = in.color.rgb / max(in.color.a, 1e-6);
    let luminance = dot(straight, vec3<f32>(0.2126, 0.7152, 0.0722));
    c = c + globals.contrast * c * (1.0 - c) * (1.0 - luminance);
    if (globals.gamma != 1.0) {
        c = pow(c, 1.0 / globals.gamma);
    }
    return in.color * clamp(c, 0.0, 1.0);
}
