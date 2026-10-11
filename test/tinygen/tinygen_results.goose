// The tinygen backend: multiple results (the first as the C return value,
// the others through out-pointers after the parameters) of scalars,
// structs and arrays, received with `let a, b = f()`, forwarded with
// `return f()`, and taken one at a time where a value is wanted.
// test/tinygen_backend.py also builds it with callers and callees left to
// C, so that the convention is the C backend's in both directions.

struct Vec2 { x: f64, y: f64 }

fn divmod(a: i64, b: i64) -> i64, i64 {
    return a / b, a % b;
}

fn minmax(xs: i32[:]) -> i32, i32, i64 {
    var lo = xs[0];
    var hi = xs[0];
    for x in xs {
        if x < lo { lo = x; }
        if x > hi { hi = x; }
    }
    return lo, hi, xs.len;
}

fn split(v: Vec2) -> f64, Vec2, bool {
    return v.x + v.y, Vec2 { x: v.y, y: v.x }, v.x > v.y;
}

fn corners(n: u8) -> u8[3], u16 {
    return [n, n + 1, n + 2], n as u16 * 300;
}

// Forwards all of divmod's results.
fn divmod10(a: i64) -> i64, i64 {
    return divmod(a, 10);
}

// Adapts them: i32 results widened to i64 ones.
fn widen(a: i32, b: i32) -> i64, i64 {
    return a, b;
}

fn main() {
    let q, r = divmod(47, 5);
    print(q, " ", r);
    let arr: i32[5] = [4, -2, 9, 0, 7];
    let lo, hi, n = minmax(arr);
    print(lo, " ", hi, " ", n);
    let s, w, gt = split(Vec2 { x: 1.5, y: 0.25 });
    print(s, " ", w.x, " ", w.y, " ", gt);
    let a, k = corners(10);
    print(a[0], " ", a[1], " ", a[2], " ", k);
    let d, m = divmod10(1234);
    print(d, " ", m);
    let x, y = widen(-3, 4);
    print(x * y);
    // Only the first result where a value is wanted; a call statement
    // discards them all.
    print(divmod(100, 7) + 1);
    divmod(1, 1);
    var total = 0;
    for i in 1..6 {
        let qq, rr = divmod(i * 13, 4);
        total += qq * 10 + rr;
    }
    print(total);
}
