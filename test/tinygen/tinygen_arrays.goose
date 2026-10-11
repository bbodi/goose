// The tinygen backend's second stage: fixed arrays and slices -- as
// values, fields, parameters and results, indexed with the bounds checks,
// sliced, measured with len, walked by for loops by value, by reference
// and with an index -- and the elements of both: scalars and structs.

struct P2 { x: i32, y: i32 }
struct Poly { pts: P2[4], n: i64 }

fn sum(s: i64[:]) -> i64 {
    var t = 0;
    for x in s { t += x; }
    return t;
}

fn weighted(s: i64[:]) -> i64 {
    var t = 0;
    for x, i in s { t += x * (i + 1); }
    return t;
}

fn bump(s: i64[:], by: i64) {
    for &x in s { x += by; }
}

fn first_last(a: i64[5]) -> i64 { a[0] * 100 + a[a.len - 1] }

fn reverse(a: i64[5]) -> i64[5] {
    var r = a;
    for i in 0..5 { r[i] = a[4 - i]; }
    return r;
}

fn perimeter(p: Poly&) -> i64 {
    var t = 0;
    for i in 0..p.n {
        let a = p.pts[i];
        let b = p.pts[(i + 1) % p.n];
        t += abs_i(a.x - b.x) + abs_i(a.y - b.y);
    }
    return t;
}

fn abs_i(v: i32) -> i32 { if v < 0 { -v } else { v } }

fn count_bytes(s: u8[:], c: u8) -> i64 {
    var n = 0;
    for b in s { if b == c { n++; } }
    return n;
}

fn at(s: i64[:], i: i64) -> i64 { s[i] }

fn main() {
    var a = [3, 1, 4, 1, 5];
    print(sum(a), " ", weighted(a), " ", first_last(a), " ", a.len);
    bump(a, 10);
    print(a[0], " ", a[4], " ", sum(a[1..3]), " ", sum(a[2..]), " ", a[..2].len);
    let r = reverse(a);
    print(r[0], " ", r[1], " ", r[2], " ", r[3], " ", r[4]);
    var z = [7; 20];
    z[19] = 1;
    print(sum(z), " ", z.len);
    var poly = Poly { pts: [P2 { x: 0, y: 0 }, P2 { x: 4, y: 0 }, P2 { x: 4, y: 3 }, P2 { x: 0, y: 3 }], n: 4 };
    print(perimeter(poly));
    poly.pts[2].y = 5;
    poly.pts[3].y = 5;
    print(perimeter(poly), " ", poly.pts[2].x);
    for &q in poly.pts { q.x *= 2; }
    print(perimeter(poly));
    print(count_bytes("hello, world", 108), " ", count_bytes("", 1));
    var total = 0;
    for x, i in [10, 20, 30] { total += x * i; }
    print(total);
    print(at(a, 4));
    print(at(a, 5));
}
