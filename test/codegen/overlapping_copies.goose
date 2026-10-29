// Byte copies whose source lies next to, or inside, their destination
// (TgFn::CopyDisjoint against tyg_move_dyn): a run copied past the end of
// the array it comes from, under and over 16 and 256 bytes, must read back
// as it was, and a run moved within its own array (a limited array set to
// its own tail, a slice regrown in a pool, a value slid down a stack) as
// memmove moves it. Equal and unequal runs of each length compare as they
// are (gs_memeq).

fn sum(s: const u8[:]) -> i64 {
    var t: i64 = 0;
    for i in s.len { t = t * 31 + s[i] + i; t = t % 1000000007; }
    return t;
}

fn label(n: i64) -> u8[>..] {
    var tmp: u8[>..] = [];
    for i in n { tmp.push('a' + (i % 26) as u8); }
    return str("<", tmp, ">");
}

fn main() {
    // A builder doubled from its own bytes, then from a window of them.
    var s: u8[>..] = [];
    s.append("0123456789");
    for r in 7 {
        s.append(s[..]);
        print(s.len, " ", sum(s));
    }
    for k in [1, 3, 8, 15, 16, 17, 100, 255, 256, 257, 600] {
        let n0 = s.len;
        s.append(s[n0 - k - 5 .. n0 - 5]);
        print(k, " ", sum(s[n0..]), " ", s[n0..] == s[n0 - k - 5 .. n0 - 5]);
    }
    // format of a builder into itself.
    var f: u8[>..] = [];
    f.append("ab");
    for r in 6 { format(f, f, "-", f.len); }
    print(f.len, " ", sum(f));

    // A limited array: appended from itself, then set to its own tail.
    var l: u8[..1024] = [];
    l.append("xyz");
    for r in 8 { l.append(l[..]); }
    print(l.len, " ", sum(l));
    for k in [1, 7, 16, 33, 300] {
        l = l[k..];
        print(l.len, " ", sum(l));
    }
    var w: i32[..64] = [];
    for i in 20 { w.push(i as i32); }
    w.append(w[3..9]);
    w = w[2..];
    print(w);

    // Runs regrown in a pool: in place, and moved past a neighbour.
    reusable[] var pool: i64[>..] = [];
    var a = pool.alloc_slice(3);
    for i in 3 { a[i] = i + 1; }
    let b = pool.alloc_slice(40);
    for i in 40 { b[i] = 100 + i; }
    a = pool.realloc_slice(a, 50);
    var t: i64 = 0;
    for i in 50 { t = t * 7 + a[i]; t = t % 1000003; }
    print(a.len, " ", t, " ", b[39], " ", pool.len);
    pool.free_slice(a);
    let c = pool.realloc_slice(b, 90);
    t = 0;
    for i in 90 { t = t * 7 + c[i]; t = t % 1000003; }
    print(c.len, " ", t, " ", pool.len);
    // A run moved down into the freed run before it, which it overlaps.
    reusable[] var q: i64[>..] = [];
    let u = q.alloc_slice(10);
    let v = q.alloc_slice(10);
    for i in 10 { u[i] = i; v[i] = 50 + i; }
    let z = q.alloc_slice(5);
    q.free_slice(u);
    let v2 = q.realloc_slice(v, 18);
    print(v2, " ", z.len, " ", q.len);

    // Values built above a temporary, then slid down to their place.
    var names: u8[][>..] = [];
    for n in [0, 5, 16, 17, 40, 300] { names.push(label(n)); }
    for nm in names { print(nm.len, " ", sum(nm)); }

    // Compares of every length up to 300, equal and differing in the last byte.
    var eq = 0;
    var ne = 0;
    var x: u8[>..] = [];
    var y: u8[>..] = [];
    for n in 301 {
        if x[..] == y[..] { eq += 1; }
        x.push('a' + (n % 7) as u8);
        y.push('a' + (n % 7) as u8 + 1);
        if x[..] == y[..] { ne += 1; }
        y[n] = x[n];
    }
    print(eq, " ", ne);
}
