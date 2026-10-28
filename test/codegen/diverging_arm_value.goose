// An arm that cannot return (abort) where a value is expected: the other arms give the
// value, the diverging one gives none. Covers if/else, match, a block whose last
// statement aborts, and a plain tail that aborts, all taken on their value arm so the
// program ends normally.
struct Run { start: i64, len: i64 }

enum Shape { Circle, Square, Tri }

fn pick_run(c: bool) -> Run {
    let r: Run = if c { Run { start: 3, len: 4 } } else { abort("no run") };
    return r;
}

fn pick_num(c: bool) -> i64 {
    let n: i64 = if c { 11 } else { abort("no number") };
    return n + 1;
}

fn nested(a: bool, b: bool) -> i64 {
    return if a { if b { 5 } else { abort("inner") } } else { abort("outer") };
}

fn sides(s: Shape) -> i64 {
    let n: i64 = match s {
        Circle => 0,
        Square => 4,
        Tri => abort("no triangles"),
    };
    return n;
}

fn sides_run(s: Shape) -> Run {
    let r: Run = match s {
        Circle => abort("no circles"),
        Square => Run { start: 1, len: 4 },
        Tri => Run { start: 2, len: 3 },
    };
    return r;
}

fn block_value(c: bool) -> i64 {
    let n: i64 = block {
        if c { break 8; }
        abort("block fell through");
    };
    return n;
}

fn block_run(c: bool) -> Run {
    let r: Run = block {
        if c { break Run { start: 9, len: 1 }; }
        abort("block fell through");
    };
    return r;
}

// The same where the value is built in place: a resizable, a frame object (a struct with
// a resizable tail) and a plain `Run` kept in a variable that is read back later.
struct Fo { n: i64, items: i64[>..] }

fn grow_if(c: bool) -> i64 {
    let xs: i64[>..] = if c { [4, 5, 6] } else { abort("no array") };
    return xs.len;
}

fn grow_match(s: Shape) -> i64 {
    let xs: i64[>..] = match s {
        Circle => abort("no circles"),
        Square => [1, 2],
        Tri => [1, 2, 3],
    };
    return xs.len;
}

fn frame_if(c: bool) -> Fo {
    var fo: Fo = if c { Fo { n: 7, items: [1, 2] } } else { abort("no frame object") };
    fo.items.push(3);
    return fo;
}

fn frame_match(s: Shape) -> Fo {
    var fo: Fo = match s {
        Circle => Fo { n: 1, items: [] },
        Square => abort("no squares"),
        Tri => Fo { n: 3, items: [7, 8, 9] },
    };
    fo.items.push(4);
    return fo;
}

fn frame_block(c: bool) -> Fo {
    let fo: Fo = block {
        if c { break Fo { n: 5, items: [6] }; }
        abort("block fell through");
    };
    return fo;
}

fn main() {
    let r = pick_run(true);
    print(r.start, " ", r.len);
    print(pick_num(true));
    print(nested(true, true));
    print(sides(Shape.Square), " ", sides(Shape.Circle));
    let q = sides_run(Shape.Tri);
    print(q.start, " ", q.len);
    print(block_value(true));
    let b = block_run(true);
    print(b.start, " ", b.len);
    print(grow_if(true), " ", grow_match(Shape.Tri));
    let fi = frame_if(true);
    print(fi.n, " ", fi.items.len);
    let fm = frame_match(Shape.Tri);
    print(fm.n, " ", fm.items.len, " ", fm.items[3]);
    let fb = frame_block(true);
    print(fb.n, " ", fb.items.len);
}
