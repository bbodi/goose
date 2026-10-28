// A call's variable-size result (`T[]`) appended to a limited array (§3.3,
// §4.1): only its elements join the run, and the append aborts instead of
// growing when they exceed the capacity. Grow-only and grow-shrink results
// and receivers go the same way.
fn triple() -> i64[] { return [1, 2, 3]; }
fn growing(n: i64) -> i64[>..] {
    var g: i64[>..] = [];
    for i in n { g.push(i * 10); }
    return g;
}
fn main() {
    var limited: i64[..] = [..8];
    limited.push(0);
    limited.append(triple());
    print(limited.len, " ", limited[3]);
    var more: i64[..] = [..8];
    more.append(growing(3));
    print(more.len, " ", more[2]);
    var shrinking: i64[>..<] = [];
    shrinking.append(triple());
    print(shrinking.len);
}
