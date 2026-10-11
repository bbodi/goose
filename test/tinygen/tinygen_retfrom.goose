// The tinygen backend's third stage: `return ... from` (§7.9), as the C
// backend does it -- the values in the target's thread-local channels,
// the discriminant in gs_rf, every frame between returning at once with
// its stacks restored -- so that a return passes through frames of
// either half: a scalar from deep recursion, a struct, two results, a
// forwarded call's results, two targets, frames that own stacks on the
// way, a plain `return` in a function value, and a recursive target.

struct P2 { x: i32, y: i32 }

recursive fn dig(d: i64, limit: i64) -> i64 {
    if d == limit { return d * 100 from search; }
    return dig(d + 1, limit) + 1;
}
fn search(limit: i64) -> i64 {
    let r = dig(0, limit);
    return r;
}

fn bad(x: i32) { if x < 0 { return P2 { x: -1, y: x } from point; } }
fn check(x: i32, y: i32) -> i32 {
    bad(x);
    bad(y);
    return x + y;
}
fn point(x: i32, y: i32) -> P2 {
    let s = check(x, y);
    return P2 { x: s, y: s * 2 };
}

fn split_fail(n: i64) { if n % 2 == 1 { return n / 2, false from split; } }
fn split(n: i64) -> i64, bool {
    split_fail(n);
    return n / 2, true;
}
fn give() -> i64, bool { return 77, true; }
fn forward() { return give() from fwd; }
fn fwd() -> i64, bool {
    forward();
    return 0, false;
}

fn inner_leave(k: i64) {
    if k == 1 { return 11 from inner; }
    if k == 2 { return 22 from outer; }
}
fn inner(k: i64) -> i64 {
    inner_leave(k);
    return 1;
}
fn outer(k: i64) -> i64 {
    let a = inner(k);
    return a + 1000;
}

fn deep2(n: i64) {
    var scratch: i64[>..] = [n, n + 1];
    scratch.push(n + 2);
    if n > 2 { return scratch.len * 10 + n from owner; }
}
fn deep(n: i64) {
    var scratch: i64[>..] = [n];
    deep2(n + 3);
    print(999);
}
fn owner(n: i64) -> i64 {
    var mine: i64[>..] = [1, 2, 3];
    deep(n);
    return -1;
}
fn after_owner() -> i64 {
    var v: i64[>..] = [];
    for i in 0..4 { v.push(i); }
    let r = owner(0);
    v.push(r);
    var s: i64 = 0;
    for x in v { s += x; }
    return s;
}

fn each<F>(xs: i64[:]) {
    for x in xs { F(x); }
}
fn first_over(xs: i64[:], k: i64) -> i64 {
    each(xs) {
        if it > k { return it; }
    }
    return -1;
}

recursive fn nest(d: i64) -> i64 {
    if d == 0 { leave_nest(); }
    return nest(d - 1) + 10;
}
fn leave_nest() { return 5 from nest; }

fn main() {
    print(search(4));
    print(search(0));
    let p = point(3, 4);
    print(p.x, p.y);
    let q = point(3, -4);
    print(q.x, q.y);
    let h, ok = split(10);
    print(h, ok);
    let h2, ok2 = split(7);
    print(h2, ok2);
    let f, fok = fwd();
    print(f, fok);
    print(outer(0));
    print(outer(1));
    print(outer(2));
    print(after_owner());
    let xs: i64[5] = [3, 1, 8, 2, 9];
    print(first_over(xs, 5));
    print(first_over(xs, 9));
    print(nest(3));
}
