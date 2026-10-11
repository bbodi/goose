// The tinygen backend's first stage: fixed structs by value and through
// references, nested, built by literals, passed to and returned from
// functions -- in both directions across the C calling convention, since
// test/tinygen_backend.py also builds a version whose callers stay in C.

struct Vec2 { x: f64, y: f64 }
struct Box { min: Vec2, max: Vec2, tag: u8 }
struct Mixed { a: i8, b: i64, c: f32, d: bool, e: u16 }

fn add(a: Vec2, b: Vec2) -> Vec2 { Vec2 { x: a.x + b.x, y: a.y + b.y } }
fn scale(v: Vec2, k: f64) -> Vec2 { Vec2 { x: v.x * k, y: v.y * k } }
fn dot(a: Vec2, b: Vec2) -> f64 { a.x * b.x + a.y * b.y }

fn area(r: Box) -> f64 { (r.max.x - r.min.x) * (r.max.y - r.min.y) }

fn grow(r: Box&, by: f64) {
    r.min.x -= by;
    r.min.y -= by;
    r.max = add(r.max, Vec2 { x: by, y: by });
    r.tag++;
}

fn mixed(n: i64) -> Mixed {
    Mixed { a: -3, b: n * 1000, c: 0.25, d: n > 2, e: 65535 }
}

fn mixed_sum(m: Mixed) -> f64 {
    var s = m.b as f64 + m.c + m.a;
    if m.d { s += 1.0; }
    return s + m.e;
}

fn swap(a: i64&, b: i64&) {
    let t = a;
    a = b;
    b = t;
}

fn main() {
    let v = add(Vec2 { x: 1.5, y: 2.0 }, scale(Vec2 { x: 1.0, y: -1.0 }, 3.0));
    print(v.x, " ", v.y, " ", dot(v, v));
    var r = Box { min: Vec2 { x: 0.0, y: 0.0 }, max: Vec2 { x: 4.0, y: 2.0 }, tag: 7 };
    print(area(r), " ", r.tag);
    grow(r, 0.5);
    print(area(r), " ", r.min.x, " ", r.max.y, " ", r.tag);
    let copy = r;
    grow(r, 1.0);
    print(area(copy), " ", area(r));
    let m = mixed(3);
    print(m.a, " ", m.b, " ", m.c, " ", m.d, " ", m.e, " ", mixed_sum(m), " ", mixed_sum(mixed(1)));
    var a = 1;
    var b = 2;
    swap(a, b);
    print(a, " ", b);
}
