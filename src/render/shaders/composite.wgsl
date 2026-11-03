// Composites a layer (an opacity group drawn offscreen) into the target below it. The quad
// covers the layer's bounds in whole physical pixels, which map 1:1 onto the layer
// texture's texels, so they are read with textureLoad: no filtering, the same everywhere.
// The texture holds linear premultiplied color (sRGB-encoded storage), scaled by opacity.

struct Globals {
    viewport: vec2<f32>,
    _text: vec2<f32>,
    origin: vec2<f32>,
    _pad: vec2<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;
@group(1) @binding(0) var layer: texture_2d<f32>;

struct Instance {
    @location(0) rect: vec4<f32>,   // the layer's bounds, physical px
    @location(1) params: vec4<f32>, // unused
    @location(2) color: vec4<f32>,  // opacity in every component
    @location(3) unused: vec4<f32>,
};

struct VsOut {
    @builtin(position) position: vec4<f32>,
    @location(0) texel: vec2<f32>,
    @location(1) opacity: f32,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32, inst: Instance) -> VsOut {
    let corner = vec2<f32>(f32(vertex & 1u), f32(vertex >> 1u));
    let p = inst.rect.xy + corner * inst.rect.zw - globals.origin;
    var out: VsOut;
    out.position = vec4<f32>(p.x / globals.viewport.x * 2.0 - 1.0,
                             1.0 - p.y / globals.viewport.y * 2.0, 0.0, 1.0);
    out.texel = corner * inst.rect.zw;
    out.opacity = inst.color.a;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Fragments sit at pixel centers, so floor() gives the texel this pixel maps to.
    return textureLoad(layer, vec2<i32>(floor(in.texel)), 0) * in.opacity;
}
