// Optionals the tinygen backend compiles (§3.8), as the C backend has
// them: a T? is the address a T& would be, null where empty (a resizable
// array's fat reference with a null header). Optional parameters,
// results, locals, struct fields, free variables and globals; made from
// a reference, a variable, `null` and `default`; tested by `if`, `while`,
// `guard`, `assert`, `== null`, `!= null`, `!` and `&&`; used narrowed,
// read and written through; rebound with `.=`; compared with `.==`;
// returned with `return from`; and printed.

struct P { x: i64, y: i64 }
struct Link { val: i64, next: Link? }
struct Slot { name: u8, at: P? }

var picked: P? = null;
var origin = P { x: 0, y: 0 };
var pts: P[3] = [P { x: 1, y: 1 }, P { x: 2, y: 4 }, P { x: 3, y: 9 }];
let NONE: Slot? = null;
var gv: i64[>..] = [5];
var gopt: i64[>..]? = null;
var table = [4, 5, 6];

fn first_big(a: i64[]&, min: i64) -> i64? {
    for &x in a { if x >= min { return x; } }
    return null;
}

fn or_zero(o: i64?) -> i64 {
    if o { return o; }
    return 0;
}

fn bump(o: i64?) {
    guard o else { print("bump: none"); return; }
    o += 1;
}

fn sum(head: Link?) -> i64 {
    var n .= head;
    var s = 0;
    while n {
        s += n.val;
        n .= n.next;
    }
    return s;
}

fn count(head: Link?) -> i64 {
    var n = 0;
    var cur .= head;
    while cur != null {
        n++;
        cur .= cur.next;
    }
    return n;
}

fn longer(a: Link?, b: Link?) -> Link? { if count(a) >= count(b) { a } else { b } }

fn shift(s: Slot&) -> bool {
    if !s.at { return false; }
    let at .= s.at;
    assert(at);
    at.x += 10;
    return true;
}

fn same(a: P?, b: P?) -> bool { a .== b }

fn pick(want: i64) {
    picked .= null;
    for &q in pts {
        if q.x == want { picked .= q; }
    }
}

fn total(v: i64[>..]?) -> i64 {
    guard v else { return -1; }
    var t = 0;
    for x in v { t += x; }
    v.push(t);
    return t;
}

fn grow(v: i64[>..]?) { if v { v.push(v.len); } }

recursive fn walk(n: Link?) -> i64 { if n { n.val + walk(n.next) } else { 0 } }

fn search(k: i64) -> i64? {
    fn look() { for &x in table { if x == k { return x from search; } } }
    look();
    return null;
}

fn find(k: i64) -> i64 {
    let r = search(k);
    if r { return r + 100; }
    return -1;
}

fn positive(head: Link?) -> i64 {
    var t = 0;
    var cur .= head;
    while cur && cur.val > 0 {
        t += cur.val;
        cur .= cur.next;
    }
    return t;
}

fn label(s: u8[]?) -> i64 { if s == null { 0 } else { s.len } }

fn counted() -> i64 {
    var seen: i64? = null;
    var n = 5;
    fn see() { if seen { seen += 1; } else { seen .= n; } }
    see();
    see();
    see();
    return n;
}

fn main() {
    var a: i64[] = [3, 9, 4, 12];
    let big = first_big(a, 9);
    if big { print("big ", big); }
    let none = first_big(a, 100);
    if none == null { print("none") }
    print(or_zero(big), " ", or_zero(none), " ", or_zero(null));
    print(big, " ", none);

    var n = 41;
    var opt: i64? = n;
    bump(opt);
    bump(null);
    print(n);
    opt .= null;
    print(opt == null, " ", opt != null, " ", !opt);
    opt .= n;
    print(opt != null && opt > 40);

    var c = Link { val: 3, next: null };
    var b = Link { val: 2, next: c };
    var l = Link { val: 1, next: b };
    print(sum(l), " ", count(l), " ", sum(null), " ", count(b));
    let lg = longer(b, l);
    if lg { print("longer starts at ", lg.val); }
    b.next .= null;
    print(sum(l), " ", count(l), " ", walk(l));
    c.val = -3;
    b.next .= c;
    print(positive(l), " ", walk(l));

    var s = Slot { name: 7 };
    print(shift(s), " ", s.at == null);
    var p = P { x: 1, y: 2 };
    s.at .= p;
    print(shift(s), " ", p.x);
    print(s);
    s.at .= null;
    print(s);

    var q = P { x: 1, y: 2 };
    print(same(p, p), " ", same(p, q), " ", same(null, null), " ", p .== q, " ", p .!= q);

    pick(2);
    if picked { print("picked ", picked.y); }
    pick(5);
    print(picked == null);
    picked .= origin;
    if picked { picked.y = 6; }
    print(origin);

    var v: i64[>..] = [1, 2, 3];
    print(total(v), " ", v.len, " ", total(null));
    let w: u8[] = "hello";
    print(label(w), " ", label(null));
    var u: i64[>..] = [];
    var ou: i64[>..]? = u;
    grow(ou);
    grow(ou);
    grow(null);
    gopt .= gv;
    grow(gopt);
    print(u, " ", gv, " ", gopt);
    fn clear() { ou .= null; }
    clear();
    print(ou == null, " ", ou, " ", NONE == null);
    print(find(5), " ", find(9));

    print(counted());
    var d = default<P?>();
    print(d == null);
    var k = 1;
    let e: i64? = if k > 0 { k } else { null };
    print(e);
}
