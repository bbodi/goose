// Values rendered structurally by print and str: structs, fixed arrays,
// slices and resizable arrays of them, nested, through references, with
// u8 arrays quoted inside a larger value and written as they are at the
// top -- the text the C backend's render functions make.

struct Vec2 { x: f64, y: f64 }
struct Tag { name: u8[4], id: u16, on: bool, w: f32 }
struct Box { min: Vec2, max: Vec2, tags: Tag[2], ns: i64[3] }
struct Link { to: Vec2&, n: i8 }

fn show(b: Box&, s: i64[:]) {
    print("box ", b, " s=", s, " ", b.ns[1]);
}

fn label(t: Tag, s: i64[:]) -> u8[>..] {
    return str(t, "/", s, "/", t.name);
}

fn points(n: i64) -> Vec2[>..] {
    var ps: Vec2[>..] = [];
    for i in 0..n { ps.push(Vec2 { x: i, y: -0.5 * i }); }
    return ps;
}

fn link(v: Vec2&) {
    let l = Link { to: v, n: -3 };
    print(l, " ", v);
}

fn main() {
    var arr = [1, -2, 3];
    let b = Box { min: Vec2 { x: 1.5, y: -2.0 }, max: Vec2 { x: 3.0, y: 4.25 },
                  tags: [Tag { name: [97, 98, 99, 100], id: 7, on: true, w: 0.5 },
                         Tag { name: [120, 34, 121, 10], id: 65535, on: false, w: 1.0e10 }],
                  ns: [9, 8, -7] };
    show(b, arr[0..2]);
    print(label(b.tags[0], arr));
    print(arr, arr[1..1]);
    print(b.min, " ", b.tags[1].name);
    let ps = points(3);
    print(ps, " ", ps.len, " ", 18446744073709551615 as u64);
    print([[1, 2], [3, 4]], [true, false], [0.25 as f32]);
    var v = Vec2 { x: 7.0, y: 8.0 };
    link(v);
}
