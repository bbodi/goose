// The tinygen backend's first stage: extern fn calls into C (§7.10), to
// libm and to the C this fixture brings (tinygen_extern_impl.c, named by
// tinygen_extern.build): scalars, a string literal as a u8 slice, arrays
// and parts of them as slices, a packed
// struct by value both ways, and a struct filled through a reference. The
// tinygen object reaches each through a trampoline the C half defines.

struct Pair { a: i32, b: f64 }

extern fn hypot(x: f64, y: f64) -> f64;
extern "abs" fn c_abs(x: i32) -> i32;
extern fn tg_checksum(s: const u8[:]) -> u32;
extern fn tg_pair_scale(p: Pair, k: i32) -> Pair;
extern fn tg_pair_fill(out: Pair&, n: i64);
extern fn tg_sum(s: const i64[:]) -> i64;

fn main() {
    print(hypot(3.0, 4.0), " ", c_abs(-42));
    print(tg_checksum("hello, tinygen"), " ", tg_checksum(""));
    let p = tg_pair_scale(Pair { a: 7, b: 1.5 }, 3);
    print(p.a, " ", p.b);
    var q = Pair { a: 0, b: 0.0 };
    tg_pair_fill(q, 12);
    print(q.a, " ", q.b);
    let xs = [5, 6, 7, 8];
    let bs: u8[3] = [104, 105, 33];
    print(tg_sum(xs), " ", tg_sum(xs[1..3]), " ", tg_checksum(bs), " ", tg_checksum(bs[..1]));
}
