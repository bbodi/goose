// The tinygen backend's third stage: globals of resizable and u8[] types,
// which live on dedicated stacks of their own in the C half's instance
// (gs_globals_t), reached through accessors it exports: read, indexed,
// iterated, pushed to, appended to, popped, passed on by reference and by
// value, written in place, and assigned anew.

struct P2 { x: i32, y: i32 }

var LOG: i64[>..] = [1, 2];
var PTS: P2[>..] = [];
var TEXT: u8[>..] = "log:";
let NAME: u8[] = "goose";
var BUF: u8[] = "abcd";

fn record(x: i64) {
    LOG.push(x);
    TEXT.append(" ");
    TEXT.push((48 + x) as u8);
}

fn sum(v: i64[>..]&) -> i64 {
    var s: i64 = 0;
    for x in v { s += x; }
    return s;
}

fn copied(v: i64[>..]) -> i64 {
    v.push(1000);
    return sum(v);
}

fn add_point(x: i32) -> i64 {
    PTS.push(P2 { x: x, y: x * 2 });
    var t: i64 = 0;
    for &p in PTS { p.y += 1; t += p.y; }
    return t;
}

fn name_len() -> i64 { NAME.len * 10 + NAME[0] }

fn shout() {
    for i in 0..BUF.len { BUF[i] -= 32; }
}

fn main() {
    record(3);
    record(4);
    print(LOG.len);
    print(sum(LOG));
    print(copied(copy(LOG)));
    print(LOG.len);
    print(LOG.pop());
    print(TEXT);
    print(add_point(1));
    print(add_point(5));
    print(name_len());
    shout();
    print(BUF);
    print(LOG[1]);
    var r: i64[>..] = [];
    print(reassign(r));
    print(sum(r));
    print(sum(LOG));
    print(TEXT);
}

// Whole resizables assigned anew: a local, a global, through a reference.
fn reassign(v: i64[>..]&) -> i64 {
    var w: i64[>..] = [1, 2, 3];
    w = [7, 8];
    w.push(9);
    v = copy(w);
    v.push(10);
    LOG = [5, 6, 7];
    TEXT = "fresh";
    return w.len * 100 + v.len * 10 + LOG.len;
}
