// A T[] or T[..] local that the returns hand back lies on the receiver's stack from its declaration
// (§7.3), in a function and in its inlined form, with the receiver's own elements under it. A return
// of another value is built behind the local and slid down over it.
fn counted(n: i64) -> i64[] {
    var out: i64[] = [n, n + 1];
    if n > 5 { return [0, 0, 0]; }
    return out;
}
fn capped(n: i64) -> i64[..] {
    var out: i64[..] = [..n + 1];
    for i in n { out.push(i * 10); }
    if n == 3 { return [7, 8]; }
    return out;
}
fn tail_value(n: i64) -> i64[..] {
    var out: i64[..] = [..4];
    out.push(n);
    out
}
fn main() {
    var all: i64[>..] = [99];
    for k in 0..8 {
        all.append(counted(k));
        all.append(capped(k));
    }
    all.append(tail_value(5));
    var sum = 0;
    for v in all { sum = (sum * 3 + v) % 1000003; }
    print(all.len, " ", sum);
    let c = counted(2);
    let d = counted(9);
    let e = capped(3);
    let f = capped(2);
    print(c[0], c[1], " ", d.len, " ", e.len, e[1], " ", f.len, f[1]);
}
