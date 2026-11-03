// Instanced rounded rectangles whose corners and border sides differ (ui_dl_border), drawn as
// CSS draws them. Coordinates are physical pixels; colors are linear and premultiplied.
// - The outer edge is a rounded box with one circular radius per corner (already scaled
//   down on the CPU so adjacent radii fit each side).
// - The inner edge (the padding box) has elliptical corners: each is the outer radius minus
//   the adjacent border widths, per axis, so a corner between a thin and a thick side bends
//   as an ellipse.
// - A pixel of the border takes the color of the side it is nearest to relative to that
//   side's width: the boundary between two sides is the line from the outer corner to the
//   inner one, as in CSS. The seam is anti-aliased over one pixel across that line.
// - The background covers the whole rect, under the border (background-clip: border-box).
// Boxes with one radius, width and color are drawn by rrect.wgsl, which is cheaper.

struct Globals {
    viewport: vec2<f32>,
    _text: vec2<f32>,
    origin: vec2<f32>,
    _pad: vec2<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;

struct Instance {
    @location(0) rect: vec4<f32>,   // x, y, width, height
    @location(1) radius: vec4<f32>, // top left, top right, bottom right, bottom left
    @location(2) width: vec4<f32>,  // border: top, right, bottom, left
    @location(3) color: vec4<f32>,
    @location(4) top: vec4<f32>,    // side colors
    @location(5) right: vec4<f32>,
    @location(6) bottom: vec4<f32>,
    @location(7) left: vec4<f32>,
};

struct VsOut {
    @builtin(position) position: vec4<f32>,
    @location(0) local: vec2<f32>, // pixel offset from the rect's top-left corner
    @location(1) size: vec2<f32>,
    @location(2) radius: vec4<f32>,
    @location(3) width: vec4<f32>,
    @location(4) color: vec4<f32>,
    @location(5) top: vec4<f32>,
    @location(6) right: vec4<f32>,
    @location(7) bottom: vec4<f32>,
    @location(8) left: vec4<f32>,
};

const AA_MARGIN: f32 = 1.0;

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32, inst: Instance) -> VsOut {
    let corner = vec2<f32>(f32(vertex & 1u), f32(vertex >> 1u));
    let local = corner * (inst.rect.zw + 2.0 * AA_MARGIN) - AA_MARGIN;
    let p = inst.rect.xy + local - globals.origin;
    var out: VsOut;
    out.position = vec4<f32>(p.x / globals.viewport.x * 2.0 - 1.0,
                             1.0 - p.y / globals.viewport.y * 2.0, 0.0, 1.0);
    out.local = local;
    out.size = inst.rect.zw;
    out.radius = inst.radius;
    out.width = inst.width;
    out.color = inst.color;
    out.top = inst.top;
    out.right = inst.right;
    out.bottom = inst.bottom;
    out.left = inst.left;
    return out;
}

// Signed distance to a box [lo, hi] whose corners are elliptical with radii rx (per corner:
// top left, top right, bottom right, bottom left) along x and ry along y. Negative inside.
// A point is in a corner's region when it is within that corner's radii of both its edges;
// regions don't overlap, since adjacent radii fit their side. There the distance is the
// implicit ellipse's value over its gradient (exact on the curve, which is all the one-pixel
// anti-aliasing ramp needs), bounded by the edges. Elsewhere it is the distance to the edges.
fn corner_box(p: vec2<f32>, lo: vec2<f32>, hi: vec2<f32>, rx: vec4<f32>, ry: vec4<f32>) -> f32 {
    let edges = max(max(lo.x - p.x, p.x - hi.x), max(lo.y - p.y, p.y - hi.y));
    // Outward distance past each corner's ellipse center, per axis.
    let left = lo.x - p.x;
    let right = p.x - hi.x;
    let top = lo.y - p.y;
    let bottom = p.y - hi.y;
    var d = vec2<f32>(0.0);
    var r = vec2<f32>(0.0);
    if (left + rx[0] > 0.0 && top + ry[0] > 0.0) {
        d = vec2<f32>(left + rx[0], top + ry[0]);
        r = vec2<f32>(rx[0], ry[0]);
    } else if (right + rx[1] > 0.0 && top + ry[1] > 0.0) {
        d = vec2<f32>(right + rx[1], top + ry[1]);
        r = vec2<f32>(rx[1], ry[1]);
    } else if (right + rx[2] > 0.0 && bottom + ry[2] > 0.0) {
        d = vec2<f32>(right + rx[2], bottom + ry[2]);
        r = vec2<f32>(rx[2], ry[2]);
    } else if (left + rx[3] > 0.0 && bottom + ry[3] > 0.0) {
        d = vec2<f32>(left + rx[3], bottom + ry[3]);
        r = vec2<f32>(rx[3], ry[3]);
    }
    if (r.x <= 0.0 || r.y <= 0.0) {
        return edges;
    }
    let q = d / r;
    let f = dot(q, q) - 1.0;
    let g = 2.0 * vec2<f32>(d.x / (r.x * r.x), d.y / (r.y * r.y));
    // Within the box too: an inner corner's radii can reach past the inner box's other edge.
    return max(f / max(length(g), 1e-4), edges);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let p = in.local;
    let size = in.size;
    // Distances are in physical pixels, so the anti-aliasing ramp is one wide. (Their
    // screen-space derivative would jump where a corner's region meets an edge.)
    let outer_d = corner_box(p, vec2<f32>(0.0), size, in.radius, in.radius);
    let outer = clamp(0.5 - outer_d, 0.0, 1.0);
    var out = in.color * outer;
    let bw = in.width; // top, right, bottom, left
    if (max(max(bw.x, bw.y), max(bw.z, bw.w)) <= 0.0) {
        return out;
    }
    // The inner edge: the padding box, with corners shrunk by the adjacent widths.
    let lo = vec2<f32>(bw.w, bw.x);
    let hi = size - vec2<f32>(bw.y, bw.z);
    var inner = 0.0;
    if (hi.x > lo.x && hi.y > lo.y) {
        let rx = max(in.radius - vec4<f32>(bw.w, bw.y, bw.y, bw.w), vec4<f32>(0.0));
        let ry = max(in.radius - vec4<f32>(bw.x, bw.x, bw.z, bw.z), vec4<f32>(0.0));
        let di = corner_box(p, lo, hi, rx, ry);
        inner = clamp(0.5 - di, 0.0, 1.0);
    }
    let ring = max(outer - inner, 0.0);
    // The side this pixel belongs to: nearest edge relative to that side's width. Sides
    // without width never win.
    let big = 1e9;
    let v = vec4<f32>(select(big, p.y / bw.x, bw.x > 0.0), select(big, (size.x - p.x) / bw.y, bw.y > 0.0),
                      select(big, (size.y - p.y) / bw.z, bw.z > 0.0), select(big, p.x / bw.w, bw.w > 0.0));
    var first = 0;
    for (var k = 1; k < 4; k++) {
        if (v[k] < v[first]) { first = k; }
    }
    var second = select(0, 1, first == 0);
    for (var k = 0; k < 4; k++) {
        if (k != first && v[k] < v[second]) { second = k; }
    }
    var colors = array<vec4<f32>, 4>(in.top, in.right, in.bottom, in.left);
    // A one-pixel ramp across the seam between the two nearest sides, centered on the
    // corner's diagonal. The difference of the two ratios times w1 w2 / |(w1, w2)| is the
    // pixel's distance from that line (v1 = d1 / w1 and v2 = d2 / w2 are equal on it).
    var widths = array<f32, 4>(bw.x, bw.y, bw.z, bw.w);
    let w1 = widths[first];
    let w2 = widths[second];
    var t = 1.0;
    if (w2 > 0.0) {
        let px = (v[second] - v[first]) * w1 * w2 / length(vec2<f32>(w1, w2));
        t = clamp(0.5 + px, 0.5, 1.0);
    }
    let border = colors[first] * t + colors[second] * (1.0 - t);
    out = border * ring + out * (1.0 - border.a * ring);
    return out;
}
