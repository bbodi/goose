// Pushes into a growable array that a reference parameter names, which the
// tinygen backend counts in locals (TgFn::PoolCache) and writes back before
// every call and exit: a recursive build of linked nodes, an inlined helper
// pushing through its own parameter, callees that read the array's length or
// push to it themselves between the caller's pushes, and the array reached
// as a global too. The links are u32 relative references, whose range check
// a 2 GB reservation makes unreachable (§3.9), optional ones null and not,
// tested and followed.

struct Node { v: i64, l: Node&<u32>?, r: Node&<u32>? }
struct Cell { v: i64, next: Cell&<u32 in cells>? }

var G: i64[>..] = [];
var cells: Cell[>..] = [];

recursive fn build(pool: Node[>..]&, depth: i64, v: i64) -> Node& {
    if depth == 0 { return pool.push(Node { v: v }); }
    let l: Node& = build(pool, depth - 1, v * 2);
    let r: Node& = build(pool, depth - 1, v * 2 + 1);
    return pool.push(Node { v: v, l: l, r: r });
}

recursive fn total(n: Node&) -> i64 {
    var t = n.v;
    let l = n.l;
    if l { t += total(l); }
    let r = n.r;
    if r { t += total(r); }
    return t;
}

recursive fn leaves(n: Node&) -> i64 {
    let l = n.l;
    guard l else { return 1; }
    let r = n.r;
    guard r else { return 1; }
    return leaves(l) + leaves(r);
}

fn count(xs: i64[>..]&) -> i64 { return xs.len; }
fn global_len() -> i64 { return G.len; }
fn push_two(xs: i64[>..]&, v: i64) { xs.push(v); xs.push(v + 1); }
fn small(xs: i64[>..]&, v: i64) { xs.push(v * 10); }

// Pushes interleaved with calls that read the length, push themselves, or
// read the array as the global it is.
fn fill(xs: i64[>..]&, n: i64) -> i64 {
    var seen = 0;
    for i in n {
        xs.push(i);
        if i % 3 == 0 { seen += count(xs); }
        if i % 4 == 1 { push_two(xs, 100 + i); }
        if i % 5 == 2 { seen += global_len() * 1000; }
        small(xs, i);
        xs.push(i + 7);
    }
    return seen;
}

fn chain(n: i64) {
    cells.push(Cell { v: 0 });
    for i in 1..n { cells.push(Cell { v: i, next: cells[cells.len - 1] }); }
}

fn main() {
    var pool: Node[>..] = [];
    let root: Node& = build(pool, 10, 1);
    print(pool.len, " ", total(root), " ", leaves(root), " ", root.v);
    let one: Node& = build(pool, 0, 5);
    let none = one.l;
    print(pool.len, " ", none == null, " ", leaves(one), " ", total(one));

    print(fill(G, 20), " ", G.len, " ", G[0], " ", G[G.len - 1]);
    var t = 0;
    for x in G { t = t * 3 + x; t = t % 1000003; }
    print(t);
    var local: i64[>..] = [];
    print(fill(local, 7), " ", local.len, " ", local);
    var more: i64[>..] = [];
    for r in 3 { print(fill(more, 9 + r), " ", more.len, " ", more[more.len - 1]); }

    chain(50);
    var c: Cell? = cells[49];
    var s = 0;
    var k = 0;
    while k < 100 {
        guard c else { break; }
        s += c.v;
        k += 1;
        c .= c.next;
    }
    print(cells.len, " ", k, " ", s);
}
