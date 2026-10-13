// Elementwise arithmetic the tinygen backend writes leaf by leaf into its
// destination and reads from its operands where they lie: a destination that
// is one of the operands (left, right or both), a scalar operand that is a
// leaf of the destination, an operand whose memory a later operand's call
// changes (it must be read before the call), compound assignment with and
// without such a call, fixed arrays short enough to unroll and too long to,
// elements of an array as destination and operands, and nested expressions.
import vec;

struct P { pos: float3, vel: float3 }

let G = float3 { 0.0, -0.5, 0.0 };

// Changes x and returns ones: x's old value is what an operand before the
// call must see.
fn bump(x: float3&) -> float3 {
    x.x += 100.0;
    return float3 { 1.0, 1.0, 1.0 };
}

fn bumpa(a: i64[300]&) -> i64[300] {
    a[0] += 1000;
    return [1; 300];
}

fn make() -> float3 { return float3 { 2.0, 3.0, 4.0 }; }

fn show(name: string, v: float3) {
    print(name, " ", v.x, " ", v.y, " ", v.z);
}

fn main() {
    var ps: P[>..] = [];
    for i in 3 {
        let f = i as f32;
        ps.push(P { pos: float3 { f, 10.0 + f, 0.0 }, vel: float3 { 1.0, f, -1.0 } });
    }
    for step in 2 {
        for &p in ps {
            p.vel = p.vel + G;
            p.pos = p.vel + p.pos;
        }
    }
    for p in ps {
        show("pos", p.pos);
        show("vel", p.vel);
    }

    var v = float3 { 1.0, 2.0, 3.0 };
    v = v * v.y;
    show("scaled", v);
    v = v + v;
    show("doubled", v);
    v = (v + make()) * v;
    show("nested", v);
    v = make() - v;
    show("call left", v);

    var a = float3 { 1.0, 2.0, 3.0 };
    a = a + bump(a);
    show("before call", a);
    a += bump(a);
    show("compound call", a);
    a += a;
    show("compound self", a);
    a = a / a.z;
    show("scalar leaf", a);

    var vs: float3[3] = [float3 { 1.0, 1.0, 1.0 }, float3 { 2.0, 2.0, 2.0 },
                         float3 { 3.0, 3.0, 3.0 }];
    vs[1] = vs[0] + vs[1];
    vs[2] = vs[2] - vs[2];
    vs[0] = vs[1] * vs[0];
    for e in vs { show("element", e); }

    var big: i64[300] = [0; 300];
    for i in 300 { big[i] = i; }
    big = big + big;
    big += big;
    big = big + bumpa(big);
    print(big[0], " ", big[1], " ", big[299]);
    var small: i64[4] = [1, 2, 3, 4];
    small = small * small;
    small += small;
    print(small[0], " ", small[1], " ", small[2], " ", small[3]);
}
