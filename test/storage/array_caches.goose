// Growable arrays whose count and top the tinygen backend keeps in a copy of
// their own (TgFn::PoolCache): a local from its declaration on, a global and a
// reference parameter from a function's entry. Statements that use the array
// otherwise run with the copy written back and set aside: its length read,
// a slice taken, the array returned or passed by value, a break or continue
// out of such a statement. An inlined helper pushes and appends through a
// reference bound to the array; a call builds a pushed element in place; two
// reference parameters of one type name the same array; an index type checks
// a value against a global's length. Pushed nodes whose fields are all given,
// links to references that cannot be null among them.

fn xs_next(x: u64) -> u64 {
    var v = x;
    v ^= v << 13;
    v ^= v >> 7;
    v ^= v << 17;
    v
}

fn xs_mod(x: u64, k: i64) -> i64 { (x % k as u64) as! i64 }

enum Ev {
    Tick,
    Word { id: i32, text: u8[varint] },
    Pair { a: i32, b: i16 },
}

struct Link { v: i64, l: Link&<u32>?, r: Link&<u32>? }

var pool: Ev..[>..] = [];
var items: i64[>..] = [];
type ItemId = u32 in items;
var text: u8[>..] = [];

// A local pushed in a loop, then returned.
recursive fn letters(seed: u64, n: i64) -> u8[] {
    var s: u8[>..] = [];
    var r = seed;
    for i in n {
        r = xs_next(r);
        s.push(('a' + xs_mod(r, 26)) as! u8);
    }
    return s;
}

// A local pushed in a loop of no calls, its length read after it.
recursive fn squares(n: i64) -> i64 {
    var s: i64[>..] = [];
    for i in n { s.push(i * i); }
    return s.len * 1000 + s[n - 1];
}

// Pushes interleaved with reads of the length and slices, a break and a
// continue out of statements that read the array.
recursive fn mixed(n: i64) -> i64 {
    var s: i64[>..] = [];
    var sum = 0;
    for i in n {
        s.push(i);
        if s.len % 3 == 0 { s.push(s.len * 10); continue; }
        if s.len > 20 { sum += s[s.len - 1]; break; }
        s.push(i + 100);
    }
    for x in s[..] { sum += x; }
    return sum * 1000 + s.len;
}

fn put(out: u8[>..]&, v: i64) {
    if v == 0 { out.push('0'); return; }
    var d = v;
    var tmp: u8[..24] = [];
    while d > 0 { tmp.push(('0' + d % 10) as! u8); d /= 10; }
    var i = tmp.len;
    while i > 0 { i--; out.push(tmp[i]); }
}

// A local handed to inlined helpers by reference, appended to and pushed
// into, then read whole.
recursive fn render(n: i64) -> i64, i64 {
    var out: u8[>..] = [];
    for i in n {
        out.append("[");
        put(out, i * 37);
        out.push(']');
    }
    var h = 0;
    for b in out[..] { h = h * 31 + b; }
    return h, out.len;
}

recursive fn word(start: i64, end: i64) -> u8[varint] { text[start..end] }

// A global pushed into, its element built by a call in place, the global read
// between the pushes.
recursive fn fill(n: i64) -> i64 {
    var r: u64 = 7;
    for i in n {
        r = xs_next(r);
        let k = xs_mod(r, 3);
        if k == 0 { pool.push(Ev.Tick); }
        else if k == 1 {
            let at = text.len;
            for c in letters(r, 1 + xs_mod(r >> 8, 6))[..] { text.push(c); }
            pool.push(Ev.Word { id: i as i32, text: word(at, text.len) });
        } else { pool.push(Ev.Pair { a: i as i32, b: (i % 100) as i16 }); }
        if pool.len % 16 == 0 { text.push(' '); }
    }
    return pool.len;
}

recursive fn scan() -> i64 {
    var t = 0;
    for e in pool {
        t += (match e {
            Tick => 1,
            Word &w => w.id + w.text.len + w.text[0],
            Pair p => p.a + p.b,
        });
    }
    return t;
}

// A global pushed into, then a value checked against its length through an
// index type, which names it without a variable.
recursive fn last_item(n: i64) -> i64 {
    for i in n { items.push(i * 3); }
    let k = (n - 1) as ItemId;
    return items[k];
}

// Two references of one type, the same array in the second call.
recursive fn two(xs: i64[>..]&, ys: i64[>..]&, n: i64) {
    for i in n {
        xs.push(i);
        ys.push(i * 2);
    }
}

recursive fn sum_of(v: i64[:]) -> i64 {
    var t = 0;
    for x in v { t += x; }
    return t;
}

recursive fn tree(links: Link[>..]&, depth: i64, v: i64) -> Link& {
    if depth == 0 { return links.push(Link { v: v, l: null, r: null }); }
    let l: Link& = tree(links, depth - 1, v * 2);
    let r: Link& = tree(links, depth - 1, v * 2 + 1);
    return links.push(Link { v: v, l: l, r: r });
}

recursive fn walk(n: Link&) -> i64 {
    var t = n.v;
    let l = n.l;
    if l { t += walk(l); }
    let r = n.r;
    if r { t += walk(r); }
    return t;
}

fn main() {
    print(letters(5, 12));
    print(squares(9), " ", mixed(5), " ", mixed(40));
    let h, len = render(30);
    print(h, " ", len);
    print(fill(200), " ", text.len, " ", scan());
    print(last_item(6), " ", last_item(2));
    var a: i64[>..] = [];
    var b: i64[>..] = [];
    two(a, b, 4);
    two(a, a, 3);
    print(a, " ", b);
    var c: i64[>..] = [];
    for i in 5 { c.push(i * i); }
    print(sum_of(c), " ", c.len);
    var links: Link[>..] = [];
    let root: Link& = tree(links, 6, 1);
    print(walk(root), " ", links.len);
}
