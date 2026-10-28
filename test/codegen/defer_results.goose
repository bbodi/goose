// A return's value is built before the defers run, and the defers run before the
// storage declared in their block is released (§6.6): a text, a resizable and
// two results come out as written whatever a defer body does with the stacks.

var log_count = 0;

fn text(n: i64) -> u8[>..] {
    var scratch: i64[>..] = [n, n + 1];
    defer log_count += scratch.len;
    defer {
        var note: u8[>..] = str("text ", scratch[1]);
        print(note);
    }
    if n < 0 { return "negative"; }
    return str("value ", scratch[0]);
}

fn numbers(n: i64) -> i64[>..] {
    var out: i64[>..] = [];
    defer print("numbers ", n);
    for i in 0..n {
        defer log_count++;
        if i == 1 { continue; }
        out.push(i * i);
    }
    return out;
}

fn pair(n: i64) -> i64, u8[>..] {
    var tag: u8[>..] = "tag";
    defer print("pair ", tag);
    return n + 1, str(tag, n);
}

fn main() {
    print(text(1));
    print(text(-1));
    print(numbers(4));
    let a, b = pair(7);
    print(a, " ", b);
    print("log ", log_count);
}
