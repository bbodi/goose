// ADTs the tinygen backend compiles: fixed-mode values built from
// variant literals and payload-less constants, variants by value and by
// reference, adapted to their ADT where one is wanted, stored in fields,
// arrays and locals and overwritten with another variant; `match` over a
// value and through a reference, binding payloads by value, several
// variants per arm and `_`; case functions dispatched on a tag, with the
// scrutinee in any position; and C-like enums of a u8 tag.

enum Shape {
    Circle { r: f64 },
    Rect { w: f64, h: f64 },
    Tri { a: f64, b: f64, c: f64 },
    Dot,
}

enum Dir { North, East, South, West }

enum Tok {
    Num { v: i64 },
    Op { c: u8 },
    Pair { a: i32, b: i16 },
    End,
}

struct Holder { id: i32, s: Shape, d: Dir }

fn area(s: Shape.Circle) -> f64 { 3.0 * s.r * s.r }
fn area(s: Shape.Rect) -> f64 { s.w * s.h }
fn area(s: Shape.Tri) -> f64 { s.a + s.b + s.c }
fn area(s: Shape.Dot) -> f64 { 0.0 }

// Dispatch on the second argument, with a shared first one.
fn scale_area(k: f64, s: Shape.Circle) -> f64 { k * area(s) }
fn scale_area(k: f64, s: Shape.Rect) -> f64 { k * s.w * s.h }
fn scale_area(k: f64, s: Shape.Tri) -> f64 { k }
fn scale_area(k: f64, s: Shape.Dot) -> f64 { -k }

fn name(s: Shape) -> i64 {
    match s {
        Circle c => if c.r > 1.0 { 10 } else { 11 },
        Rect r => if r.w == r.h { 20 } else { 21 },
        _ => 99,
    }
}

fn scaled(s: Shape, k: f64) -> Shape {
    match s {
        Circle c => Shape.Circle { r: c.r * k },
        Rect r => Shape.Rect { w: r.w * k, h: r.h * k },
        Tri t => Shape.Tri { a: t.a * k, b: t.b * k, c: t.c * k },
        Dot => Shape.Dot,
    }
}

fn by_ref(s: Shape&) -> f64 {
    match s {
        Circle c => c.r,
        Rect r => r.w + r.h,
        Tri, Dot => -1.0,
    }
}

fn grow(s: Shape&) {
    s = match s {
        Circle c => Shape.Circle { r: c.r + 1.0 },
        _ => Shape.Dot,
    };
}

fn circle(r: f64) -> Shape.Circle { Shape.Circle { r: r } }
fn widen(c: Shape.Circle&) { c.r *= 2.0; }

fn turn(d: Dir) -> Dir {
    match d {
        North => Dir.East,
        East => Dir.South,
        South => Dir.West,
        West => Dir.North,
    }
}

fn dirnum(d: Dir) -> i64 { match d { North, South => 1, East, West => 2 } }

fn tokval(t: Tok) -> i64 {
    match t {
        Num n => n.v,
        Op o => o.c,
        Pair p => p.a as i64 * 1000 + p.b,
        End => -1,
    }
}

fn first_tok(ts: Tok[:]) -> Tok { ts[0] }

fn main() {
    let c = Shape.Circle { r: 2.0 };
    let r: Shape = Shape.Rect { w: 3.0, h: 4.0 };
    var s: Shape = c;
    print(area(s), " ", area(r), " ", name(s), " ", name(r));
    s = Shape.Tri { a: 1.0, b: 2.0, c: 3.0 };
    print(area(s), " ", name(s), " ", scale_area(2.0, s));
    s = Shape.Dot;
    print(area(s), " ", name(s), " ", scale_area(2.0, s), " ", by_ref(s));
    s = scaled(r, 2.0);
    print(area(s), " ", by_ref(s), " ", name(Shape.Rect { w: 5.0, h: 5.0 }));
    var cc = circle(1.5);
    widen(cc);
    print(area(cc), " ", cc.r, " ", name(cc), " ", scale_area(1.0, circle(1.0)));
    var t: Shape = Shape.Circle { r: 0.5 };
    grow(t);
    grow(t);
    print(by_ref(t));
    var u: Shape = Shape.Rect { w: 1.0, h: 1.0 };
    grow(u);
    print(by_ref(u), " ", area(u));

    var h = Holder { id: 7, s: Shape.Circle { r: 3.0 }, d: Dir.West };
    print(h.id, " ", area(h.s), " ", dirnum(h.d));
    h.s = Shape.Rect { w: 2.0, h: 0.5 };
    h.d = turn(h.d);
    print(area(h.s), " ", dirnum(h.d), " ", name(h.s));

    var arr: Shape[3] = [Shape.Dot, Shape.Circle { r: 1.0 }, Shape.Rect { w: 2.0, h: 2.0 }];
    var total = 0.0;
    for x in arr { total += area(x); }
    arr[0] = scaled(arr[2], 3.0);
    for &x in arr { total += by_ref(x); }
    print(total, " ", name(arr[0]));

    var d = Dir.North;
    var turns = 0;
    loop {
        d = turn(d);
        turns++;
        if dirnum(d) == 1 && turns > 2 { break; }
    }
    print(turns, " ", dirnum(d));

    let ts: Tok[4] = [Tok.Num { v: 42 }, Tok.Op { c: 43 }, Tok.Pair { a: 3, b: -4 }, Tok.End];
    var sum = 0;
    for x in ts { sum += tokval(x); }
    print(sum, " ", tokval(first_tok(ts)), " ", tokval(ts[2]));
    let k = match ts[1] {
        Op o => if o.c == 43 { 1 } else { 2 },
        _ => 3,
    };
    print(k);
}
