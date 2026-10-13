// Control expressions the tinygen backend compiles: `block` with its
// tail's value or a break's, a break out of a block inside a loop and a
// continue passing a block by, `loop` with a value, integer `match` (single
// values, ranges, several per arm, at the ends of signed and unsigned
// types, first or not, as a value and as a statement), and
// references rebound with `.=`.

struct P { x: i64, y: i64 }

fn classify(x: i64) -> i64 {
    match x {
        0 => 10,
        1, 2, 3 => 20,
        4..10 => 30,
        -9223372036854775808 => 40,
        9223372036854775807 => 50,
        -5..0, 100..200 => 60,
        _ => 0,
    }
}

fn wide(x: u64) -> i64 {
    match x {
        18446744073709551615 => 1,
        9223372036854775808..18446744073709551615 => 2,
        _ => 3,
    }
}

fn small(x: u8) -> i64 { match x { 255 => 1, 0 => 2, 200..255 => 3, _ => 4 } }
fn tiny(x: i8) -> i64 { match x { 127 => 1, -128 => 2, -127..0 => 3, _ => 4 } }

fn side(x: i64) {
    match x {
        1 => print("one"),
        2 => { print("two"); },
        _ => {},
    }
}

fn exits(c: i64) -> i64 {
    var n: i64;
    block {
        if c == 0 { n = 1; break; }
        if c == 1 { n = 2; break; }
        n = 3;
    }
    return n;
}

fn valued(c: bool) -> i64 {
    var n: i64;
    let v = block {
        if c { break { n = 1; 4 }; }
        n = 2;
        5
    };
    return v + n;
}

fn point(c: i64) -> P {
    return block {
        if c > 2 { break P { x: c, y: -c }; }
        P { x: 0, y: c }
    };
}

fn first_over(a: i64[:], k: i64) -> i64 {
    var i = 0;
    let r = loop {
        if i == a.len { break -1; }
        if a[i] > k { break a[i]; }
        i++;
    };
    return r;
}

fn skips(a: i64[:]) -> i64 {
    var s = 0;
    for v in a {
        block {
            if v < 0 { break; }
            if v == 0 { continue; }
            s += v;
        }
        s += 1000;
    }
    return s;
}

fn larger(a: i64&, b: i64&) -> i64 {
    var r .= a;
    if b > a { r .= b; }
    r += 1;
    return r;
}

fn swap_points(c: bool) -> i64 {
    var p = P { x: 1, y: 2 };
    var q = P { x: 3, y: 4 };
    var r .= p;
    if c { r .= q; }
    r.x = 9;
    return p.x * 10 + q.x;
}

fn main() {
    let xs: i64[12] = [0, 2, 3, 7, 10, -9223372036854775807 - 1, 9223372036854775807, -3, 150, 200, -6, 99];
    for x in xs { print(x, " -> ", classify(x)); }
    print(wide(18446744073709551615), wide(9223372036854775808), wide(5));
    print(small(255), small(0), small(201), small(7), " ", tiny(127), tiny(-128), tiny(-1), tiny(0));
    side(1);
    side(2);
    side(3);
    print(exits(0), exits(1), exits(2), " ", valued(true), " ", valued(false));
    print(point(1).y, " ", point(5).x, point(5).y);
    let a = [3, 8, -2, 0, 11, 5];
    print(first_over(a, 7), " ", first_over(a, 20), " ", skips(a));
    var u = 4;
    var w = 6;
    print(larger(u, w), " ", u, " ", w, " ", larger(w, u), " ", w);
    print(swap_points(false), " ", swap_points(true));
    let m = (match u { 4 => 1, _ => 2 }) + (block { break 40; });
    print(m);
}
