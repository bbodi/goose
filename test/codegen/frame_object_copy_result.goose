// copy() of a frame object, a struct with a resizable tail (C.2), given as a
// function's result: the copy owns its tail's elements, so growing the
// original afterwards leaves the copy as it was.
struct Frame { depth: i64, marks: i64[>..] }
fn snapshot(f: Frame&) -> Frame { return copy(f); }
fn main() {
    var f = Frame { depth: 1, marks: [4, 5] };
    let s = snapshot(f);
    f.marks.push(6);
    f.depth = 2;
    print(s.depth, " ", s.marks.len, " ", f.depth, " ", f.marks.len);
}
