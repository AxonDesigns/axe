// Instanced analytic rounded rectangles with an optional border. Coordinates are physical
// pixels; colors are linear and premultiplied. Coverage comes from the signed distance and
// its screen-space derivative, so edges get a one-pixel ramp at any (fractional) scale.
// As in CSS, the background covers the whole rect and the border is drawn over it, inside
// the edge; the inner edge's radius is the outer radius minus the border width.

struct Globals {
    viewport: vec2<f32>, // size of the target this pass draws into
    _text: vec2<f32>,    // text contrast and gamma (glyph.wgsl)
    origin: vec2<f32>,   // physical px of the target's top-left (a layer's bounds)
    _pad: vec2<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;

struct Instance {
    @location(0) rect: vec4<f32>,         // x, y, width, height
    @location(1) params: vec4<f32>,       // radius, border width, reserved
    @location(2) color: vec4<f32>,
    @location(3) border_color: vec4<f32>,
};

struct VsOut {
    @builtin(position) position: vec4<f32>,
    @location(0) local: vec2<f32>,  // pixel offset from the rect center
    @location(1) half_size: vec2<f32>,
    @location(2) radius: f32,
    @location(3) border: f32,
    @location(4) color: vec4<f32>,
    @location(5) border_color: vec4<f32>,
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
    let p = center + local - globals.origin;
    var out: VsOut;
    out.position = vec4<f32>(p.x / globals.viewport.x * 2.0 - 1.0,
                             1.0 - p.y / globals.viewport.y * 2.0, 0.0, 1.0);
    out.local = local;
    out.half_size = half_size;
    out.radius = min(inst.params.x, min(half_size.x, half_size.y));
    out.border = inst.params.y;
    out.color = inst.color;
    out.border_color = inst.border_color;
    return out;
}

fn rounded_box(p: vec2<f32>, half_size: vec2<f32>, radius: f32) -> f32 {
    let q = abs(p) - half_size + vec2<f32>(radius);
    return length(max(q, vec2<f32>(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let d = rounded_box(in.local, in.half_size, in.radius);
    let w = max(fwidth(d), 1e-4);
    let outer = clamp(0.5 - d / w, 0.0, 1.0);
    var out = in.color * outer;
    if (in.border > 0.0) {
        // Inside the inner edge there is no border; a border thicker than half the rect
        // leaves no inside at all.
        let inner_half = in.half_size - vec2<f32>(in.border);
        var inner = 0.0;
        if (inner_half.x > 0.0 && inner_half.y > 0.0) {
            let di = rounded_box(in.local, inner_half, max(in.radius - in.border, 0.0));
            inner = clamp(0.5 - di / w, 0.0, 1.0);
        }
        let ring = max(outer - inner, 0.0);
        out = in.border_color * ring + out * (1.0 - in.border_color.a * ring);
    }
    return out;
}
