// copy() of a call that returns a reference to a frame object (a struct with a
// resizable tail): the copy owns its own tail, so growing the original later
// leaves it alone.
struct Fo { n: i64, items: i64[>..] }
fn first(f: Fo&) -> Fo& { return f; }

fn main() {
    var f = Fo { n: 1, items: [4, 5] };
    var g: Fo = copy(first(f));
    f.items.push(6);
    g.n = 7;
    print(f.n, " ", f.items.len, " ", g.n, " ", g.items.len);
}
