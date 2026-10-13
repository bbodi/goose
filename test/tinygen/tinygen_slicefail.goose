// A slice's bounds checked by the tinygen backend: the abort names the
// Goose line, as the C backend's does.

fn part(s: i64[:], lo: i64, hi: i64) -> i64 { s[lo..hi].len }

fn main() {
    let a = [1, 2, 3];
    print(part(a, 1, 3), " ", part(a, 0, 0));
    print(part(a, 2, 4));
}
