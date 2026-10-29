// Typed memory in tinygen's C (TgFn::TagAt): fields, elements and globals
// are read and written as their C types, so the C compiler may keep apart
// accesses of different types. These are the cases where it must not: an
// ADT local rewritten between variants whose payloads are different scalar
// types at the same bytes, the particles pattern (structs in an array
// updated in a loop while a global of the same field types is read),
// nested struct fields, a global written through a reference, and the
// bytes of an array read as another element type.

struct V3 { x: f32, y: f32, z: f32 }
struct Part { pos: V3, vel: V3 }
struct Outer { id: i64, inner: Part, tail: i32 }

enum Num {
    I { v: i64 },
    F { v: f64 },
    Pair { a: i32, b: f32 },
}

let G = V3 { x: 0.0, y: -0.0625, z: 0.0 };
var W = V3 { x: 1.0, y: 2.0, z: 3.0 };

fn bump(v: V3&) { v.x = v.x + 1.0; }

fn flip(n: i64) -> i64 {
    var e: Num = Num.I { v: 1 };
    var acc: f64 = 0.0;
    for i in n {
        match e {
            I p => { acc += p.v; e = Num.F { v: p.v + 0.5 }; },
            F p => { acc += p.v; e = Num.Pair { a: i as i32, b: 0.25 }; },
            Pair p => { acc += p.a + p.b; e = Num.I { v: i * 3 }; },
        }
    }
    return (acc * 4.0) as! i64;
}

fn particles(n: i64) -> i64 {
    var ps: Part[>..] = [];
    for i in n {
        let f = i as f32;
        ps.push(Part { pos: V3 { x: f, y: f + 1.0, z: 2.0 }, vel: V3 { x: 0.5, y: 0.0, z: -0.5 } });
    }
    for s in 50 {
        for &p in ps {
            p.vel = p.vel + G;
            p.pos = p.pos + p.vel;
            if p.pos.y < 0.0 {
                p.pos.y = 0.0 - p.pos.y;
                p.vel.y = 0.0 - p.vel.y * 0.5;
            }
        }
    }
    var t: i64 = 0;
    for &p in ps { t += (p.pos.x * 16.0) as! i64 + (p.pos.y * 16.0) as! i64 + (p.pos.z * 16.0) as! i64; }
    return t;
}

fn nested(n: i64) -> i64 {
    var os: Outer[4] = [Outer { id: 0, inner: Part { pos: V3 { x: 0.0, y: 0.0, z: 0.0 }, vel: V3 { x: 1.0, y: 2.0, z: 3.0 } }, tail: 0 }; 4];
    for i in n {
        let k = i % 4;
        os[k].inner.pos.x = os[k].inner.pos.x + os[k].inner.vel.y;
        os[k].inner.vel = os[(k + 1) % 4].inner.pos;
        os[k].id += 1;
        os[k].tail = os[k].tail + os[k].id as i32;
        let copy = os[k].inner;
        os[(k + 2) % 4].inner.pos = copy.vel;
    }
    var t: i64 = 0;
    for o in os { t += o.id + o.tail + (o.inner.pos.x * 8.0) as! i64 + (o.inner.vel.z * 8.0) as! i64; }
    return t;
}

fn through_ref(n: i64) -> f32 {
    var s: f32 = 0.0;
    for i in n {
        bump(W);
        s = s + W.x;
        W.x = W.x - 0.5;
    }
    return s;
}

fn reinterpreted() {
    var a: i64[>..] = [4607182418800017408, 4611686018427387904];
    let v, ok = from_bytes<f64[>..]>(to_bytes(a));
    a[0] = 4613937818241073152;
    print(ok, " ", v.len, " ", a[0]);
    for x in v { print(x); }
}

fn main() {
    print(flip(1000));
    print(particles(1000));
    print(nested(1000));
    print(through_ref(100));
    reinterpreted();
}
