// The tinygen backend's limited arrays: static-capacity T[..k] as locals,
// fields, elements, parameters (by value and by reference), results and
// globals; built from literals, strings, fills, slices and other arrays
// (copied in under the capacity check); indexed, sliced, iterated,
// compared and printed; grown with push, append and format (into a field
// with no builder) and shrunk with pop, pop_n, resize and clear; and ending
// in a push past the capacity, which must abort with its Goose line.

struct Name { tag: i32, text: u8[..8] }

struct Bag { items: i64[..4][..3], count: i16 }

var history: i64[..6] = [1, 2];

fn digits(n: u64) -> u8[..20] {
    var out: u8[..20] = [];
    var x = n;
    loop {
        out.push((x % 10) as u8 + '0');
        x /= 10;
        if x == 0 { break; }
    }
    var r: u8[..20] = [];
    for i in 0 .. out.len { r.push(out[out.len - 1 - i]); }
    return r;
}

fn sum(a: i64[..8]) -> i64 {
    var s = 0;
    for x in a { s += x; }
    return s;
}

fn grow(a: i64[..8]&, n: i64) -> i64& {
    for i in 0 .. n { a.push(i * 10); }
    return a.push(99);
}

fn shrink(a: i64[..8]&) -> i64 {
    let last = a.pop();
    a.pop_n(1);
    return last * 100 + a.len;
}

fn halves(s: u8[:]) -> u8[..16] {
    var h: u8[..16] = s[0 .. s.len / 2];
    h.append("|");
    h.append(s[s.len / 2 ..]);
    return h;
}

fn named(t: i32, s: u8[:]) -> Name {
    return Name { tag: t, text: s };
}

fn bag() -> Bag {
    var b = Bag { items: [], count: 0 };
    b.items.push([1, 2]);
    b.items.push([3]);
    b.items[1].push(4);
    b.count = b.items.len as i16;
    return b;
}

fn main() {
    print(digits(9045), " ", digits(0), " ", digits(18446744073709551615));
    var a: i64[..8] = [5, 6, 7];
    print(sum(a), " ", a.len, " ", a.cap);
    var r = grow(a, 2);
    r += 1;
    print(a, " ", a.len);
    print(shrink(a), " ", a);
    a.resize(6, -1);
    print(a);
    a.resize(2);
    print(a, " ", a[1]);
    a.clear();
    print(a.len, " ", a);
    let f: i64[..8] = [3; 4];
    print(f, " ", sum(f));
    let fixed: i64[5] = [9, 8, 7, 6, 5];
    a = fixed;
    print(a);
    a = fixed[1 .. 3];
    print(a, " ", a == fixed[1 .. 3], " ", a != [8, 7]);
    print(halves("abcdef"), " ", halves("xy"));
    let n = named(7, "tiny");
    print(n, " ", n.text.len, " ", n.text);
    var names: Name[..3] = [];
    names.push(n);
    names.push(named(8, "two"));
    for &m in names { m.tag += 100; }
    print(names);
    let b = bag();
    print(b.items, " ", b.count, " ", b.items[1][1]);
    var s: u8[..8] = "hey";
    s.append(['!', '!']);
    s[0] = 'H';
    print(s, " ", s[1 .. 3], " ", s == "Hey!!");
    history.push(3);
    history.append([4, 5]);
    print(history, " ", history.len);
    let c = card(3, true, "new");
    print(c.cost, "|", c.tag, " ", c.tag.len, " ", card(-12, false, "").cost);
    var small: u8[..4] = "abcd";
    print(small.len);
    small.pop();
    small.push('z');
    print(small);
    small.push('!');
    print("not reached");
}

struct Card { cost: u8[..48], tag: u8[..8] }

fn card(r: i64, rare: bool, name: u8[:]) -> Card {
    var c = Card { cost: [], tag: [] };
    format(c.cost, "R", r, " ", rare, " ", 2.5, " ", 0.5 as f32, " ", 18446744073709551615 as u64,
           " ", if rare { "rare" } else { "always" });
    format(c.tag, name);
    format(c.tag, "!", -7 as i8);
    return c;
}
