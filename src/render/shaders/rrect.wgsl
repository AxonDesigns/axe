// Instanced analytic rounded rectangles. Coordinates are physical pixels; colors are linear
// and premultiplied. Coverage comes from the signed distance and its screen-space
// derivative, so edges get a one-pixel ramp at any (fractional) scale.

struct Globals {
    viewport: vec2<f32>,
    _pad: vec2<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;

struct Instance {
    @location(0) rect: vec4<f32>,   // x, y, width, height
    @location(1) params: vec4<f32>, // radius, reserved
    @location(2) color: vec4<f32>,
};

struct VsOut {
    @builtin(position) position: vec4<f32>,
    @location(0) local: vec2<f32>,  // pixel offset from the rect center
    @location(1) half_size: vec2<f32>,
    @location(2) radius: f32,
    @location(3) color: vec4<f32>,
};

// One pixel of margin so the anti-aliasing ramp outside the edge is rasterized.
const AA_MARGIN: f32 = 1.0;

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32, inst: Instance) -> VsOut {
    // Triangle strip corners: (0,0) (1,0) (0,1) (1,1).
    let corner = vec2<f32>(f32(vertex & 1u), f32(vertex >> 1u));
    let half_size = inst.rect.zw * 0.5;
    let center = inst.rect.xy + half_size;
    let local = (corner * 2.0 - 1.0) * (half_size + AA_MARGIN);
    let p = center + local;
    var out: VsOut;
    out.position = vec4<f32>(p.x / globals.viewport.x * 2.0 - 1.0,
                             1.0 - p.y / globals.viewport.y * 2.0, 0.0, 1.0);
    out.local = local;
    out.half_size = half_size;
    out.radius = min(inst.params.x, min(half_size.x, half_size.y));
    out.color = inst.color;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let q = abs(in.local) - in.half_size + vec2<f32>(in.radius);
    let d = length(max(q, vec2<f32>(0.0))) + min(max(q.x, q.y), 0.0) - in.radius;
    let w = max(fwidth(d), 1e-4);
    let coverage = clamp(0.5 - d / w, 0.0, 1.0);
    return in.color * coverage;
}
