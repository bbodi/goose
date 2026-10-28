// Calls the optimizer inlines at -O1 and -O2 (an InlineBlock): a `return`
// inside the inlined body leaves the block with its value, from nested
// blocks, loops and inlined bodies of other functions, whatever the value's
// type; recursion and early returns in functions that are inlined.

struct Pair { a: i64, b: i64 }

fn clamp(x: i64, lo: i64, hi: i64) -> i64 {
    if x < lo { return lo; }
    if x > hi { return hi; }
    return x;
}

// Returns from inside a loop, and calls an inlined function itself.
fn first_over(xs: i64[:], limit: i64) -> i64 {
    for x in xs {
        if clamp(x, 0, 1000) > limit { return x; }
    }
    return -1;
}

fn sign(x: i64) -> i64 {
    if x < 0 { return -1; }
    if x == 0 { return 0; }
    return 1;
}

fn describe(x: i64) -> u8[>..] {
    if x < 0 { return str("neg", -x); }
    if x == 0 { return str("zero"); }
    return str("pos", x);
}

fn divide(a: i64, b: i64) -> Pair {
    if b == 0 { return Pair { a: 0, b: 0 }; }
    return Pair { a: a / b, b: a % b };
}

fn triple(x: i64) -> i64[3] {
    if x > 100 { return [x, x, x]; }
    return [x, x * 2, x * 3];
}

fn pick(xs: i64[:], i: i64) -> i64[:] {
    if i < 0 { return xs[0..0]; }
    return xs[i..];
}

fn bump(x: i64&, by: i64) {
    if by == 0 { return; }
    x += by;
}

recursive fn fib(n: i64) -> i64 {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

recursive fn gcd(a: i64, b: i64) -> i64 {
    if b == 0 { return a; }
    return gcd(b, a % b);
}

// Early returns three inlined bodies deep.
fn grade(x: i64) -> i64 {
    if sign(x) < 0 { return clamp(x, -5, 0); }
    return clamp(x, 0, 9) + sign(x);
}

fn main() {
    let xs: i64[5] = [3, 40, 7, 2000, 9];
    print(first_over(xs[..], 10), " ", first_over(xs[..], 5000), " ", first_over(xs[..2], 1));
    print(sign(-7), " ", sign(0), " ", sign(12), " ", clamp(50, 0, 10), " ", clamp(-3, 0, 10));
    print(describe(-4), " ", describe(0), " ", describe(8));
    let d = divide(17, 5);
    let z = divide(1, 0);
    print(d.a, " ", d.b, " ", z.a, " ", z.b);
    let t1 = triple(4);
    let t2 = triple(400);
    print(t1[0] + t1[1] + t1[2], " ", t2[2]);
    print(pick(xs[..], 3).len, " ", pick(xs[..], -1).len, " ", pick(xs[..], 3)[0]);
    var v = 10;
    bump(v, 5);
    bump(v, 0);
    print(v);
    print(fib(15), " ", gcd(48, 18), " ", gcd(7, 0));
    var total = 0;
    for i in -3..12 { total += grade(i); }
    print(total);
}
