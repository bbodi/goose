// The tinygen backend's third stage: the resizable-array builtins that
// shrink or reshape an array -- pop, pop_n, resize (shrinking, and growing
// by copies of a fill value, scalar and struct), clear -- through a
// reference and on locals, assert, and ending in a pop of an empty array,
// which must abort with its Goose line as the C backend's does.

struct P2 { x: i32, y: i32 }

fn shrink(v: i64[>..]&) -> i64 {
    let a = v.pop();
    v.pop_n(2);
    return a * 100 + v.len;
}

fn sized(n: i64) -> i64[>..] {
    var v: i64[>..] = [1, 2, 3, 4, 5, 6];
    v.resize(n, -1);
    v.resize(v.len - 1);
    return v;
}

fn points() -> P2[>..] {
    var ps: P2[>..] = [];
    ps.resize(3, P2 { x: 1, y: 2 });
    let q = ps.pop();
    ps.push(P2 { x: q.y, y: q.x });
    return ps;
}

fn reset(s: u8[>..]&) -> i64 {
    let n = s.len;
    s.clear();
    s.append("new");
    assert(s.len == 3);
    return n;
}

fn show(v: i64[>..]&) {
    var line: u8[>..] = "[";
    for x, i in v {
        if i > 0 { line.append(" "); }
        format(line, x);
    }
    line.append("]");
    print(line);
}

fn main() {
    var v: i64[>..] = [10, 20, 30, 40, 50];
    print(shrink(v), " ", v.len);
    show(v);
    var a = sized(2);
    show(a);
    var b = sized(9);
    show(b);
    let ps = points();
    print(ps.len, " ", ps[2].x, ps[2].y, " ", ps[0].x);
    var s: u8[>..] = "old string";
    print(reset(s), " ", s);
    var e: i64[>..] = [7];
    print(e.pop());
    e.pop();
    print("not reached");
}
