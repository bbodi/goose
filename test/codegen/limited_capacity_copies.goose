// A variable-size or growable array that was copied (copy(), §4.1) becomes a
// limited array whose capacity is chosen at construction (`T[..]`, §3.3): in
// a local, as an argument and as a result. The capacity is the copy's
// length, so the limited array is full until it shrinks.
fn total(a: i64[..]) -> i64 {
    var sum = 0;
    for x in a { sum += x; }
    return sum;
}
fn growable_copy() -> i64[..] {
    var g: i64[>..] = [10, 20, 30];
    g.push(40);
    return copy(g);
}
fn main() {
    let v: i64[] = [1, 2, 3];
    var local: i64[..] = copy(v);
    print(local.len, " ", local.cap, " ", total(copy(local)));

    var g: i64[>..] = [4, 5];
    g.push(6);
    print(total(copy(g)), " ", total(copy(v)));

    var result = growable_copy();
    print(result.len, " ", total(copy(result)));
    result.pop();
    print(result.len, " ", result.cap, " ", total(copy(result)));
}
