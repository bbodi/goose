// The tinygen backend's third stage: function values (§7.6), whose
// checked body is spliced in at each call with its parameters bound as
// locals -- `it` and named ones, scalars, structs, slices and resizable
// arrays, a body with locals and nested functions of its own, function
// values inside function values, one passed down a recursion, and one
// called for its effect alone.

struct P2 { x: i32, y: i32 }

fn apply<F>(n: i64) -> i64 { F(n) }
fn both<F>(a: i64, b: i64) -> i64 { F(a) * 100 + F(b) }
fn pair<F>(a: i64, b: i64) -> i64 { F(a, b) }
fn mapp<F>(p: P2) -> P2 { F(p) }
fn sum_by<F>(xs: i64[:]) -> i64 {
    var s: i64 = 0;
    for x in xs { s += F(x); }
    return s;
}
fn fill<F>(v: i64[>..]&, n: i64) {
    for i in 0..n { F(v, i); }
}
fn each<F>(xs: i64[:]) {
    for x in xs { F(x); }
}
recursive fn countdown<F>(k: i64) {
    if k > 0 { countdown(k - 1, F); }
    F(k);
}

fn main() {
    let base: i64 = 100;
    print(apply(4) { it * 3 });
    print(apply(4) { let k = it + 1; base + k });
    print(both(3, 4) { x => x * x });
    print(pair(7, 2) { a, b => a * 10 - b });
    let q = mapp(P2 { x: 1, y: 2 }) { p => P2 { x: p.y * 10, y: p.x } };
    print(q.x);
    print(q.y);
    let xs: i64[4] = [1, 2, 3, 4];
    print(sum_by(xs) { it * it });
    print(apply(4) { fn g() -> i64 { it * 2 + base } g() });
    print(apply(4) { x => apply(3) { y => x * 10 + y } });
    var v: i64[>..] = [];
    fill(v, 4) { a, i => a.push(i * base); };
    for e in v { print(e); }
    var total: i64 = 0;
    each(xs) { total += it; };
    print(total);
    countdown(3) { print(it); };
}
