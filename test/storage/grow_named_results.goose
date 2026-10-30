// A resizable local that every return of its function hands back as a T[]
// is built at the result's destination, behind the count the return then
// writes (TgFn::NrvoBind's img): bytes and variable-size elements, counts
// past one varint byte, a destination of another count width (an
// Item[varint] field built by a call returning an Item[]), other values
// returned while the local is open, inlined and out-of-line callees.

struct Item { name: u8[varint], qty: varint }
struct Order { id: varint, items: Item[varint] }
struct Wide { id: varint, items: Item[] }

fn letters(n: i64) -> u8[] {
    var s: u8[>..] = [];
    for i in n { s.push(('a' + i % 26) as! u8); }
    return s;
}

recursive fn letters_rec(n: i64) -> u8[] {
    var s: u8[>..] = [];
    for i in n { s.push(('A' + i % 26) as! u8); }
    return s;
}

fn items(n: i64) -> Item[] {
    var t: Item[>..] = [];
    for i in n { t.push(Item { name: letters(1 + i % 5), qty: i * 7 - 3 }); }
    return t;
}

recursive fn items_rec(n: i64) -> Item[] {
    var t: Item[>..] = [];
    for i in n { t.push(Item { name: letters_rec(1 + i % 3), qty: 1000 * i }); }
    return t;
}

// Other values returned while the local is open, one from inside its loop.
fn picky(n: i64) -> u8[] {
    if n < 0 { return "neg"; }
    var s: u8[>..] = [];
    for i in n {
        if i == 200 { return "cut"; }
        s.push(('0' + i % 10) as! u8);
    }
    if n == 3 { return str("three ", n); }
    return s;
}

// Every element's quantity and name length, and the last name's first letter.
fn tally(t: Item[]&) -> i64 {
    var s = 0;
    var last = 0;
    for it in t { s += it.qty + it.name.len; last = it.name[0]; }
    return s * 1000 + last;
}

fn wide_sum(w: Wide&) -> i64 {
    var t = 0;
    for it in w.items { t += it.qty + it.name.len; }
    return t;
}

fn qty_sum(o: Order&) -> i64 {
    var t = 0;
    for it in o.items { t += it.qty + it.name.len; }
    return t;
}

fn main() {
    print(letters(5), " ", letters(0), " ", letters_rec(4));
    let long = letters(300);
    print(long.len, " ", long[0], " ", long[129], " ", long[299]);
    let lr = letters_rec(200);
    print(lr.len, " ", lr[199]);
    let small = items(3);
    print(small.len, " ", tally(small));
    let o = Order { id: 9, items: items(150) };
    print(o.id, " ", o.items.len, " ", qty_sum(o));
    let w = Wide { id: 4, items: items_rec(130) };
    print(w.id, " ", w.items.len, " ", wide_sum(w));
    let r = Order { id: 1, items: items_rec(2) };
    print(r.items.len, " ", qty_sum(r));
    print(picky(-1), " ", picky(3), " ", picky(5), " ", picky(250), " ", picky(150).len);
    var e: u8[>..] = [];
    e.append(letters(3));
    e.append(picky(4));
    e.push('!');
    print(e);
}
