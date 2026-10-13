// The tinygen backend's third stage: nested functions and their free
// variables (§7.5), passed as the C backend passes them -- a fixed
// variable by its address, a u8[] by its image's, a resizable array by
// its header's and its stack -- so that either half calls the other:
// read and written, a struct's field, an array's element, a reference,
// a string, an array grown from inside, a by-value array parameter, a
// nested function's own nested function passing one on, and recursion.

struct P2 { x: i32, y: i32 }

fn counter(start: i64) -> i64 {
    var n = start;
    var calls: i32 = 0;
    fn bump(by: i64) {
        n += by;
        calls++;
    }
    bump(3);
    bump(4);
    return n * 10 + calls;
}

fn fields(k: i32) -> i32 {
    var p = P2 { x: k, y: 2 };
    let w: i64[3] = [5, 6, 7];
    fn grow() -> i32 {
        p.x += p.y;
        return p.x + w[2] as i32;
    }
    let a = grow();
    let b = grow();
    return a * 100 + b + p.x;
}

fn via(r: i64&) -> i64 {
    fn twice() { r *= 2; }
    twice();
    twice();
    return r;
}

fn words(s: u8[]) -> i64 {
    let t: u8[] = "tail";
    fn total() -> i64 { s.len + t.len }
    return total();
}

fn build(n: i64) -> i64[>..] {
    var v: i64[>..] = [];
    fn add(x: i64) { v.push(x * x); }
    for i in 0..n { add(i); }
    return v;
}

fn more(v: i64[>..]) -> i64 {
    fn add() { v.push(v.len); }
    add();
    add();
    var s: i64 = 0;
    for x in v { s += x; }
    return s;
}

fn outer(base: i64) -> i64 {
    var acc = base;
    fn mid(k: i64) -> i64 {
        fn inner() { acc += k; }
        inner();
        inner();
        return acc;
    }
    return mid(1) + mid(10);
}

fn fact(n: i64) -> i64 {
    var steps = 0;
    recursive fn go(k: i64) -> i64 {
        steps++;
        if k <= 1 { return 1; }
        return k * go(k - 1);
    }
    let f = go(n);
    return f * 100 + steps;
}

fn main() {
    print(counter(5));
    print(fields(1));
    var x: i64 = 3;
    print(via(x));
    print(x);
    print(words("head"));
    let v = build(5);
    for e in v { print(e); }
    print(more([1, 2, 3]));
    print(outer(100));
    print(fact(6));
}
