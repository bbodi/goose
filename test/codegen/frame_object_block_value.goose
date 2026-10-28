// A block that breaks with a frame object (a struct with a resizable tail): the
// value is copied out of the variable it was read from.
struct Fo { n: i64, items: i64[>..] }

fn snapshot(early: bool) -> Fo {
    var f = Fo { n: 1, items: [8, 9] };
    let g = block {
        if early { break copy(f); }
        f.n = 5;
        f.items.push(10);
        break copy(f);
    };
    f.items.push(11);
    return g;
}

fn main() {
    let a = snapshot(true);
    let b = snapshot(false);
    print(a.n, " ", a.items.len, " ", b.n, " ", b.items.len);
}
