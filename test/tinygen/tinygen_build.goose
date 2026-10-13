// The tinygen backend's third stage: functions that build strings (u8[>..])
// and resizable arrays (T[>..]) of their own -- locals on stacks the
// function owns, literals, push and append, results built at the caller's
// destination stack (alone, and beside a fixed result the C returns), by-
// value arguments copied onto a stack -- and whose scopes give those stacks
// back on every way out: the end of a block, break, continue and return.
// test/tinygen_backend.py also leaves chosen functions to C, so that the
// destinations and gs_sp cross the two halves in both directions.

struct P2 { x: i32, y: i32 }

fn squares(n: i64) -> i64[>..] {
    var v: i64[>..] = [];
    for i in 0..n { v.push(i * i); }
    return v;
}

fn word(n: i64) -> u8[>..] {
    var s: u8[>..] = [];
    for i in 0..n { s.push('a' + i as u8); }
    s
}

fn evens(n: i64) -> i64[>..] {
    var a: i64[>..] = [];
    var b: i64[>..] = [];
    for i in 0..n {
        if i % 2 == 0 { a.push(i); } else { b.push(i); }
    }
    if b.len > 100 { return a; }
    a.push(sum(b));
    return a;
}

fn sum(v: i64[>..]&) -> i64 {
    var t = 0;
    for x in v { t += x; }
    return t;
}

fn own_sum(v: i64[>..]) -> i64 {
    v.push(1000);
    return sum(v);
}

fn label(k: i64) -> u8[>..] {
    var s: u8[>..] = "k=";
    s.append(word(k));
    s.append("!");
    let tail: const u8[:] = "xyz";
    s.append(tail[1..]);
    return s;
}

fn split(n: i64) -> i64[>..], i64 {
    var v: i64[>..] = [n, n + 1];
    v.append(squares(3));
    return v, v.len * 10;
}

fn points(n: i32) -> P2[>..] {
    var ps: P2[>..] = [P2 { x: 0, y: n }];
    for i in 1..n { ps.push(P2 { x: i, y: n - i }); }
    return ps;
}

fn scan(n: i64) -> i64 {
    // Stacks taken in a loop body, given back at break, continue and the
    // body's end; and at a return from inside the loop.
    var t = 0;
    for i in 0..n {
        var w: i64[>..] = [i, i];
        let sq = squares(i);
        if i == 2 { continue; }
        if i == 6 { break; }
        if sq.len > 7 { return t + 1000; }
        t += sum(w) + sum(sq);
    }
    return t;
}

fn nested(n: i64) -> i64 {
    var total = 0;
    var outer: i64[>..] = [];
    for i in 0..n {
        outer.push(i);
        var inner: i64[>..] = [];
        inner.append(outer[0..]);
        inner.append(evens(i));
        total += sum(inner) * inner.len;
    }
    return total + outer.len;
}

fn main() {
    let v = squares(5);
    print(sum(v), " ", v.len);
    print(word(4));
    let e = evens(9);
    print(sum(e), " ", e.len, " ", own_sum(copy(e)), " ", e.len);
    print(label(3), " ", label(0));
    let s, n = split(7);
    print(sum(s), " ", s.len, " ", n);
    let ps = points(4);
    var far = 0;
    for p in ps { if p.x * p.y > far { far = p.x * p.y; } }
    print(ps.len, " ", far);
    print(scan(5), " ", scan(10), " ", scan(20));
    print(nested(6));
    var acc: u8[>..] = [];
    for i in 0..3 {
        acc.append(label(i));
        acc.push(' ');
    }
    print(acc, acc.len);
}
