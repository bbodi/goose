// A pool's base read once per function (TgFn::PoolBase) while its links are stored, tested and
// followed; null tests and stores of a link read from the same pool through its offset
// (TgFn::KnownOff); varints of one byte and of more (TgFn::UlebRead); functions the C makes static.

import std;

struct Node { key: i32, prev: Node&<u32 in pool>?, next: Node&<u32 in pool>? }

reusable var pool: Node[>..] = [];
// A global initializer whose function links into the pool built before it.
var head: i64 = start();

fn start() -> i64 {
    let h = pool.alloc_index(Node { key: -1 });
    pool[h].next .= pool[h];
    pool[h].prev .= pool[h];
    return h;
}

fn insert_after(at: Node&, n: Node&) {
    let q = at.next;
    assert(q);
    n.prev .= at;
    n.next .= q;
    q.prev .= n;
    at.next .= n;
}

fn unlink(n: Node&) {
    let p = n.prev;
    let q = n.next;
    guard p != null && q != null else { print("unlinked"); return; }
    p.next .= q;
    q.prev .= p;
    n.prev .= null;
    n.next .= null;
}

fn sum(dir: i64) -> i64 {
    var s = 0;
    var cur = if dir > 0 { pool[head].next } else { pool[head].prev };
    loop {
        guard cur else { break; }
        guard cur.key >= 0 else { break; }
        s = s * 3 + cur.key;
        cur .= if dir > 0 { cur.next } else { cur.prev };
    }
    return s;
}

enum Ev { Tick, Say { id: i32, text: u8[varint] } }

fn text(n: i64) -> u8[varint] {
    var w: u8[>..] = [];
    for i in n { w.push(('a' + i % 26) as! u8); }
    return w;
}

fn main() {
    var keep: (Node&<u32 in pool>?)[>..] = [];
    for i in 10 {
        let k = pool.alloc_index(Node { key: i as i32 });
        insert_after(pool[head], pool[k]);
        if i % 3 == 0 { keep.push(pool[k]); }
    }
    print(sum(1));
    print(sum(-1));
    for k in keep {
        let n = k;
        guard n else { continue; }
        unlink(n);
        unlink(n);
        print(n.prev == null);
        pool.free(pool.index_of(n));
    }
    print(sum(1));
    let again = pool.alloc_index(Node { key: 42 });
    insert_after(pool[head], pool[again]);
    print(sum(1));
    print(pool[head].next != null);
    let first = pool[head].next;
    assert(first);
    print(pool[pool.index_of(first)].key);

    var log: Ev..[>..] = [];
    for i in 6 {
        log.push(Ev.Say { id: i as i32, text: text(i * 60) });
        log.push(Ev.Tick);
    }
    var total = 0;
    for e in log {
        total += (match e { Tick => 1, Say &s => s.id + s.text.len + (if s.text.len > 0 { s.text[s.text.len - 1] as i64 } else { 0 }) });
    }
    print(total);
}
