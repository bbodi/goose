// The tinygen backend's first stage (docs/design/tinygen_backend.md):
// integers of every width, floats, bools, casts, and the control flow
// around them. test/tinygen_backend.py builds it with --backend=tinygen and
// requires every function to go through tinygen; the suite also runs it
// through the C backends, against the same expected output.

recursive fn fib(n: i64) -> i64 {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn collatz_steps(start: u64) -> i32 {
    var n = start;
    var steps: i32 = 0;
    while n != 1 {
        if n % 2 == 0 { n = n / 2; } else { n = 3 * n + 1; }
        steps++;
    }
    return steps;
}

fn narrow(a: i8, b: u8, c: i16, d: u16, e: i32, f: u32) {
    // Unsigned arithmetic wraps at its width; the signed sums here fit.
    let bb: u8 = b + 200;
    let dd: u16 = d * 3;
    let ff: u32 = f - 10;
    print(a + 1, " ", bb, " ", c - 2, " ", dd, " ", e * 2, " ", ff);
    print(~b, " ", b << 3, " ", b >> 1, " ", a >> 1, " ", c & 0xff, " ", d | 1, " ", e ^ 5);
}

fn divisions(a: i64, b: i64) {
    // `%` is Euclidean (§6.2): never negative.
    print(a / b, " ", a % b, " ", -a / b, " ", -a % b, " ", a / -b, " ", a % -b);
}

fn floats(x: f64, y: f32) -> f64 {
    let z = x * 2.5 - 1.0 / x;
    let w: f32 = y * y + 0.5;
    print(z, " ", w, " ", x % 3.0, " ", -x, " ", x < 3.0, " ", x >= 3.0);
    return z + w;
}

fn casts(x: i64, d: f64) {
    let a = x as i32;
    let b = x as! u8;
    let c = (d * 4.0) as i64;
    let e = d as! i32;
    let f = x as f32;
    let g = 300 as! i8;
    print(a, " ", b, " ", c, " ", e, " ", f, " ", g, " ", x as u64, " ", (x * 1000) as f64);
}

fn logic(a: bool, b: bool) -> bool {
    let c = a && !b || (b && a);
    let d = !(a || b);
    return c != d;
}

fn sum_to(n: i64) -> i64 {
    var s = 0;
    for i in 1..n + 1 { s += i; }
    var t = 0;
    for i in n {
        if i % 3 == 0 { continue; }
        if i > 50 { break; }
        t += i;
    }
    var k = 0;
    loop {
        k += 7;
        if k > 40 { break; }
    }
    return s * 1000 + t + k;
}

fn sign(x: i64) -> i64 { if x < 0 { -1 } else if x == 0 { 0 } else { 1 } }

fn bump(x: i64) -> i64 {
    var y = x;
    y += 3;
    y -= 1;
    y *= 4;
    y /= 3;
    y %= 100;
    y <<= 2;
    y >>= 1;
    y |= 1;
    y &= 0xff;
    y ^= 0x10;
    y++;
    y--;
    return y;
}

fn main() {
    var s: i64 = 0;
    for i in 0..20 { s += fib(i); }
    print("fib sum ", s, " fib(25) ", fib(25));
    print("collatz ", collatz_steps(27), " ", collatz_steps(97));
    narrow(-5, 100, -300, 40000, 123456, 5);
    divisions(17, 5);
    divisions(9223372036854775807, 2);
    print(floats(3.5, 1.25));
    casts(65537, -2.75);
    print(logic(true, false), " ", logic(false, false), " ", logic(true, true));
    print(sum_to(100), " ", sign(-8), " ", sign(0), " ", sign(12), " ", bump(17));
    let big: u64 = 18446744073709551615;
    print(big, " ", big / 3, " ", big % 1000, " ", big >> 60);
}
