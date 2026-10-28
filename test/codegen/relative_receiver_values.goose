// runtime-debug: a value with relative links is built where it stays. A link to
// another element measures from the slot it is stored in, so a call, a function
// value, an if, a match, a block or a loop that gives such a value builds it at
// the receiver's slot: a pushed or assigned element, a field of a literal, an
// element of an array literal, a result of a function, and a pool slot.
struct Node { id: i64, next: Node&<u32>? }
struct Pair { left: Node, right: Node }
struct Trio { id: i64, nodes: Node[3] }
enum Kind { A, B, C }
var pool: Node[>..] = [];

fn make(first: Node&, id: i64) -> Node { Node { id: id, next: first } }
fn choose(first: Node&, k: Kind) -> Node {
    match k {
        A => make(first, 10),
        B => if first.id == 0 { make(first, 20) } else { make(first, 21) },
        C => block { if first.id == 0 { break make(first, 30); } make(first, 31) },
    }
}
fn make_pair(first: Node&, flag: bool) -> Pair {
    Pair { left: make(first, 1), right: if flag { make(first, 2) } else { Node { id: 3, next: first } } }
}
fn make_trio(first: Node&) -> Trio {
    Trio { id: 4, nodes: [make(first, 5), Node { id: 6, next: first }, choose(first, Kind.A)] }
}
fn with_count(first: Node&) -> Node, i64 { return make(first, 40), 41; }
fn forward_count(first: Node&) -> Node, i64 { return with_count(first); }
fn give<F>() { F() }
fn show(label: u8[:], xs: Node[>..]&) {
    for &x in xs {
        let next = x.next;
        print(label, " ", x.id, " ", if next { next.id } else { -1 });
    }
}

fn main() {
    var xs: Node[>..] = [];
    xs.push(Node { id: 0 });
    let first .= xs[0];
    xs.push(choose(first, Kind.A));
    xs.push(choose(first, Kind.B));
    xs.push(choose(first, Kind.C));
    xs.push(give() { match xs.len { 4 => make(first, 50), _ => make(first, 51) } });
    xs.push(give() { loop { break make(first, 52); } });
    xs[1] = if xs.len > 3 { make(first, 60) } else { make(first, 61) };
    xs[2] = give() { make(first, 62) };
    xs.push(forward_count(first));
    show("xs", xs);

    var ps: Pair[>..] = [];
    ps.push(Pair { left: Node { id: 0 }, right: Node { id: 0 } });
    let pfirst .= ps[0].left;
    ps.push(make_pair(pfirst, true));
    ps.push(make_pair(pfirst, false));
    ps.push(make_pair(pfirst, true));
    for &pr in ps {
        let l = pr.left.next;
        let r = pr.right.next;
        print(pr.left.id, " ", if l { l.id } else { -1 }, " ", pr.right.id, " ", if r { r.id } else { -1 });
    }

    var ts: Trio[>..] = [];
    ts.push(Trio { id: 0, nodes: [Node { id: 0 }, Node { id: 0 }, Node { id: 0 }] });
    let tfirst .= ts[0].nodes[0];
    ts.push(make_trio(tfirst));
    for &t in ts {
        for &nd in t.nodes {
            let next = nd.next;
            print(t.id, " ", nd.id, " ", if next { next.id } else { -1 });
        }
    }

    pool.push(Node { id: 70 });
    let head .= pool[0];
    pool.push(choose(head, Kind.B));
    pool.push(make(head, 71));
    show("pool", pool);
}
