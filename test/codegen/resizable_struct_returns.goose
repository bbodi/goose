// A struct whose last field is a resizable ADT (a resizable struct, §5) held
// in a local and handed back by name: a `var`, a `let`, a block's tail and
// one of two returns. The result is built where the caller wants it, and the
// variant stored in the tail (with and without its own elements) survives.
enum Log { Empty, Entries { n: i64, items: i64[>..] }, Bare { n: i64 } }
struct Wrap { id: i64, log: Log.. }

fn kind(w: Wrap&) -> i64 { match w.log { Entries => 1, Empty => 0, Bare => 2 } }

fn from_var(i: i64) -> Wrap {
    var w = Wrap { id: i, log: Log.Bare { n: 2 } };
    return w;
}
fn from_let(i: i64) -> Wrap {
    let w = Wrap { id: i, log: Log.Entries { n: 1, items: [i, i] } };
    return w;
}
fn from_tail(i: i64) -> Wrap {
    var w = Wrap { id: i, log: Log.Empty };
    w
}
fn from_branch(early: bool) -> Wrap {
    var a = Wrap { id: 1, log: Log.Empty };
    if early { return a; }
    return Wrap { id: 2, log: Log.Bare { n: 1 } };
}

fn main() {
    let a = from_var(1);
    let b = from_let(2);
    let c = from_tail(3);
    let d = from_branch(true);
    let e = from_branch(false);
    print(a.id, " ", kind(a));
    print(b.id, " ", kind(b));
    print(c.id, " ", kind(c));
    print(d.id, " ", kind(d), " ", e.id, " ", kind(e));
}
