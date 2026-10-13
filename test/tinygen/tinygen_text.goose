// The tinygen backend's third stage: u8[] values built in tinygen
// functions -- locals on stacks of their own (from literals, slices, other
// strings and calls), results built whole at the caller's destination
// stack (alone and beside a fixed result), and by-value arguments copied
// onto a statement's stack -- with the C backend's conventions, so that
// test/tinygen_backend.py can leave either side to C; str and format of
// scalars and strings, and string comparisons.

fn lit() -> u8[] { return "abc"; }

fn greet(name: const u8[:]) -> u8[] {
    let hi: u8[] = "hi ";
    let who: u8[] = name;
    let again: u8[] = copy(hi);
    print(hi, who, again, " ", hi.len, " ", who[1]);
    return who;
}

fn first(s: u8[]) -> u8 { return s[0]; }

fn size(s: u8[]&) -> i64 { return s.len; }

fn pair() -> u8[], i64 { return "xy", 7; }

fn forward() -> u8[] { return lit(); }

fn named() -> u8[] {
    let a: u8[] = "local";
    return a;
}

fn tail(s: const u8[:], from: i64) -> u8[] { s[from..] }

fn wide(s: const u8[:]) -> u8[u16] { s }

fn label(n: i64, f: f64, b: bool) -> u8[] { str("n=", n, " f=", f, " b=", b) }

fn same(a: u8[]&, b: const u8[:]) -> bool { a == b }

fn differ(a: const u8[:], b: const u8[:]) -> bool { a != b }

fn width(s: const u8[:]) -> i64 { s.len }

fn listing(k: i64) -> u8[>..] {
    var out: u8[>..] = "[";
    for i in 0..k { format(out, i, ","); }
    format(out, "] ", width(str("ab", 12)), " ", 1.5 as f32, " ", 18446744073709551615 as u64, " ", -7 as i8);
    out
}

fn main() {
    let g = greet("bob");
    print(g, " ", lit(), " ", forward(), " ", named(), " ", tail("goose", 2));
    print(first(copy(g)), " ", first("zed"), " ", size(g), " ", first(lit()));
    let a, n = pair();
    print(a, n, " ", a.len);
    let e: u8[] = "";
    let w = wide("sixteen");
    print(e.len, " ", w, " ", w.len);
    for i in 0..3 {
        let t: u8[] = tail("abcdef", i);
        print(t, " ", size(t));
    }
    let s = label(42, 0.25, true);
    print(s, " ", str(1, "/", 2));
    print(same(s, "n=42 f=0.25 b=true"), same(s, "x"), differ("ab", "ab"), differ("ab", "ac"), differ("", ""));
    print(listing(4));
    let xs: i64[3] = [1, 2, 3];
    let ys: i64[3] = [1, 2, 3];
    print(xs == ys, " ", xs[0..2] == ys[1..3]);
}
