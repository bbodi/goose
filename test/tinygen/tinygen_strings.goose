// The tinygen backend's third stage, so far: strings (u8[]) and resizable
// arrays (T[>..]) handed to functions -- by reference (the image's address,
// or a fat reference: the header and its stack), and by value (an image
// the caller copied, or a header and its stack) -- read in place with len,
// indexing, slicing and for loops, printed, viewed as slices, and grown
// with push through a reference. main builds them, which only C does yet;
// test/tinygen_backend.py also leaves the callees to C, so that every kind
// crosses the two halves in both directions.

struct P2 { x: i32, y: i32 }

fn count(s: u8[]&, c: u8) -> i64 {
    var n = 0;
    for b in s { if b == c { n += 1; } }
    return n;
}

fn first_last(s: u8[]) -> i64 {
    let a: i64 = s[0];
    return a * 1000 + s[s.len - 1];
}

fn total(v: i64[>..]&) -> i64 {
    var t = 0;
    for x, i in v { t += x * (i + 1); }
    return t;
}

fn sum_view(s: i64[:]) -> i64 {
    var t = 0;
    for x in s { t += x; }
    return t;
}

fn addto(v: i64[>..]&, x: i64) {
    v.push(x);
    v.push(x * 10);
}

fn bump_all(v: i64[>..]&, by: i64) {
    for &x in v { x += by; }
    v[0] = v[0] - 1;
}

fn own(v: i64[>..], x: i64) -> i64 {
    v.push(x);
    addto(v, x + 1);
    return v.len + sum_view(v[1..]);
}

fn shout(s: u8[]&, v: u8[>..]&) {
    print("[", s, "] [", v, "] ", s.len, " ", v.len, " ", s[1..3]);
}

fn far(ps: P2[>..]&) -> i32 {
    var best: i32 = 0;
    for p in ps {
        if p.x * p.x + p.y * p.y > best { best = p.x * p.x + p.y * p.y; }
    }
    ps.push(P2 { x: best, y: 0 });
    return best;
}

fn walk(s: u8[]&, v: i64[>..]&) -> i64 {
    addto(v, count(s, 'l'));
    return total(v) + count(s, 'o');
}

fn main() {
    let s: u8[] = "hello";
    let ba: u8[] = "banana";
    print(count(s, 'l'), " ", count(ba, 'a'), " ", first_last(copy(s)));
    var v: i64[>..] = [1, 2, 3];
    print(total(v), " ", sum_view(v), " ", sum_view(v[1..]));
    addto(v, 4);
    print(v.len, " ", v);
    bump_all(v, 100);
    print(v);
    print(own(copy(v), 7), " ", v.len);
    var b: u8[>..] = [];
    b.append("builder");
    shout(s, b);
    var ps: P2[>..] = [P2 { x: 1, y: 2 }, P2 { x: -3, y: 1 }];
    print(far(ps), " ", ps.len, " ", ps[2].x);
    print(walk(s, v), " ", v.len);
}
