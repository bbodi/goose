// Fixed structs by value across the C calling convention, in both directions
// (test/tinygen_backend.py leaves chosen functions to C). Goose's structs are
// packed, so a field may sit off its natural alignment: on SysV x86-64 such a
// struct goes in memory, an aligned one in registers, and both halves must
// agree. Sizes 1 to 33 bytes, int and float fields, floats only, and more
// arguments than registers.

struct B1 { a: u8 }
struct B3 { a: u8, b: i8, c: u8 }
struct F2 { a: f32, b: f32 }
struct I8m { a: i16, b: u8, c: u8, d: i32 }
struct U8F { a: u8, b: f32 }
struct FU8 { a: f32, b: u8 }
struct IF12 { a: i32, b: f64 }
struct F3 { a: f32, b: f32, c: f32 }
struct DF12 { a: f64, b: f32 }
struct U8I64 { a: u8, b: i64 }
struct I64U8 { a: i64, b: u8 }
struct DI { a: f64, b: i64 }
struct ID { a: i64, b: f64 }
struct D2 { a: f64, b: f64 }
struct Odd16 { a: u8, b: i32, c: i16, d: f64, e: u8 }
struct S17 { a: i64, b: f64, c: u8 }
struct S24 { a: f64, b: i64, c: f64 }
struct S33 { a: u8, b: f64, c: i64, d: f32, e: i32, f: f64 }
struct Arr { a: u8, b: i32[2] }
struct Inner { x: f32, y: f32 }
struct Nest { t: u8, v: Inner }
struct Nest8 { t: i32, v: Inner }

fn mk_b1(k: i64) -> B1 { B1 { a: (200 + k) as u8 } }
fn mk_b3(k: i64) -> B3 { B3 { a: (250 + k) as u8, b: (-7 - k) as i8, c: (9 + k) as u8 } }
fn mk_f2(k: i64) -> F2 { F2 { a: 1.5 + k, b: -2.25 } }
fn mk_i8m(k: i64) -> I8m { I8m { a: (-300 - k) as i16, b: 7, c: 254, d: (-100000 - k) as i32 } }
fn mk_u8f(k: i64) -> U8F { U8F { a: (3 + k) as u8, b: 4.5 } }
fn mk_fu8(k: i64) -> FU8 { FU8 { a: -0.75, b: (5 + k) as u8 } }
fn mk_if12(k: i64) -> IF12 { IF12 { a: (-11 - k) as i32, b: 1000.125 } }
fn mk_f3(k: i64) -> F3 { F3 { a: 1.0, b: 2.0 + k, c: -3.5 } }
fn mk_df12(k: i64) -> DF12 { DF12 { a: 12345.5 + k, b: 0.5 } }
fn mk_u8i64(k: i64) -> U8I64 { U8I64 { a: 17, b: -9000000000 - k } }
fn mk_i64u8(k: i64) -> I64U8 { I64U8 { a: 8000000000 + k, b: 201 } }
fn mk_di(k: i64) -> DI { DI { a: -6.5, b: 77 + k } }
fn mk_id(k: i64) -> ID { ID { a: -78 - k, b: 6.25 } }
fn mk_d2(k: i64) -> D2 { D2 { a: 3.125, b: -4.0 - k } }
fn mk_odd16(k: i64) -> Odd16 {
    Odd16 { a: 250, b: (-123456 - k) as i32, c: -321, d: 2.5 + k, e: 66 }
}
fn mk_s17(k: i64) -> S17 { S17 { a: 1 + k, b: 2.5, c: 3 } }
fn mk_s24(k: i64) -> S24 { S24 { a: 1.25, b: -2 - k, c: 3.75 } }
fn mk_s33(k: i64) -> S33 {
    S33 { a: 9, b: -1.5, c: 123456789012 + k, d: 0.25, e: -42, f: 7.0 + k }
}
fn mk_arr(k: i64) -> Arr { Arr { a: 4, b: [(10 + k) as i32, -20] } }
fn mk_nest(k: i64) -> Nest { Nest { t: 2, v: Inner { x: 0.5 + k, y: -8.0 } } }
fn mk_nest8(k: i64) -> Nest8 { Nest8 { t: -3, v: Inner { x: 1.5, y: 9.0 + k } } }

fn h_b1(s: B1) -> f64 { s.a }
fn h_b3(s: B3) -> f64 { s.a + 3.0 * s.b + 5.0 * s.c }
fn h_f2(s: F2) -> f64 { s.a as f64 + 3.0 * s.b }
fn h_i8m(s: I8m) -> f64 { s.a + 3.0 * s.b + 5.0 * s.c + 7.0 * s.d }
fn h_u8f(s: U8F) -> f64 { s.a as f64 + 3.0 * s.b }
fn h_fu8(s: FU8) -> f64 { s.a as f64 + 3.0 * s.b }
fn h_if12(s: IF12) -> f64 { s.a + 3.0 * s.b }
fn h_f3(s: F3) -> f64 { s.a as f64 + 3.0 * s.b + 5.0 * s.c }
fn h_df12(s: DF12) -> f64 { s.a + 3.0 * s.b as f64 }
fn h_u8i64(s: U8I64) -> f64 { s.a + 3.0 * s.b }
fn h_i64u8(s: I64U8) -> f64 { s.a + 3.0 * s.b }
fn h_di(s: DI) -> f64 { s.a + 3.0 * s.b }
fn h_id(s: ID) -> f64 { s.a + 3.0 * s.b }
fn h_d2(s: D2) -> f64 { s.a + 3.0 * s.b }
fn h_odd16(s: Odd16) -> f64 {
    s.a + 3.0 * s.b + 5.0 * s.c + 7.0 * s.d + 11.0 * s.e
}
fn h_s17(s: S17) -> f64 { s.a + 3.0 * s.b + 5.0 * s.c }
fn h_s24(s: S24) -> f64 { s.a + 3.0 * s.b + 5.0 * s.c }
fn h_s33(s: S33) -> f64 {
    s.a + 3.0 * s.b + 5.0 * s.c + 7.0 * s.d + 11.0 * s.e + 13.0 * s.f
}
fn h_arr(s: Arr) -> f64 { s.a + 3.0 * s.b[0] + 5.0 * s.b[1] }
fn h_nest(s: Nest) -> f64 { s.t as f64 + 3.0 * s.v.x + 5.0 * s.v.y }
fn h_nest8(s: Nest8) -> f64 { s.t as f64 + 3.0 * s.v.x + 5.0 * s.v.y }

// Passed through: the argument comes back as the result.
fn p_b3(s: B3) -> B3 { s }
fn p_i8m(s: I8m) -> I8m { s }
fn p_u8f(s: U8F) -> U8F { s }
fn p_fu8(s: FU8) -> FU8 { s }
fn p_if12(s: IF12) -> IF12 { s }
fn p_f3(s: F3) -> F3 { s }
fn p_df12(s: DF12) -> DF12 { s }
fn p_u8i64(s: U8I64) -> U8I64 { s }
fn p_di(s: DI) -> DI { s }
fn p_id(s: ID) -> ID { s }
fn p_odd16(s: Odd16) -> Odd16 { s }
fn p_s17(s: S17) -> S17 { s }
fn p_s33(s: S33) -> S33 { s }
fn p_nest(s: Nest) -> Nest { s }
fn p_nest8(s: Nest8) -> Nest8 { s }

// Small scalars, which the caller extends.
fn small(a: i8, b: u8, c: i16, d: u16, e: bool, f: f32) -> f64 {
    var t = a as f64 + 3.0 * b + 5.0 * c + 7.0 * d + 11.0 * f;
    if e { t += 1000.0; }
    return t;
}
fn ret_i8(k: i64) -> i8 { (-100 - k) as i8 }
fn ret_u16(k: i64) -> u16 { (65000 + k) as u16 }

// More arguments than registers: six integer and eight SSE registers. Once
// one two-register struct no longer fits, it goes on the stack while the
// scalars after it still take the registers left.
fn many_int(a: i64, b: i64, c: i64, d: i64, e: i64, s: ID, f: i64, g: I64U8, h: i64) -> f64 {
    a + 2.0 * b + 3.0 * c + 4.0 * d + 5.0 * e + 6.0 * h_id(s) +
        7.0 * f + 8.0 * h_i64u8(g) + 9.0 * h
}
fn many_sse(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, s: D2, g: f64, t: F2, h: f32,
            i: f64) -> f64 {
    a + 2.0 * b + 3.0 * c + 4.0 * d + 5.0 * e + 6.0 * f + 7.0 * h_d2(s) + 8.0 * g + 9.0 * h_f2(t) +
        10.0 * h as f64 + 11.0 * i
}
fn many_mix(a: DI, b: ID, c: DI, d: S17, e: DI, f: Odd16, g: IF12, h: B3, i: F3, j: DI, k: u8,
            l: f32) -> f64 {
    h_di(a) + 2.0 * h_id(b) + 3.0 * h_di(c) + 4.0 * h_s17(d) + 5.0 * h_di(e) + 6.0 * h_odd16(f) +
        7.0 * h_if12(g) + 8.0 * h_b3(h) + 9.0 * h_f3(i) + 10.0 * h_di(j) + 11.0 * k +
        12.0 * l as f64
}
// A memory result and memory arguments past the registers.
fn many_ret(a: i64, b: i64, c: i64, d: i64, e: i64, f: S24, g: i64, h: DI) -> S33 {
    S33 { a: (a + b) as u8, b: f.a + f.c, c: c + d + e + f.b + g, d: 0.5, e: h.b as i32, f: h.a }
}
fn many_ret2(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, h: i64, i: DF12) -> DI {
    DI { a: a + b + c + d + e + f + g + i.a, b: h + 1 }
}

fn main() {
    for k in 0..2 {
        print("b1 ", h_b1(mk_b1(k)), " b3 ", h_b3(p_b3(mk_b3(k))), " f2 ", h_f2(mk_f2(k)));
        print("i8m ", h_i8m(p_i8m(mk_i8m(k))), " u8f ", h_u8f(p_u8f(mk_u8f(k))),
              " fu8 ", h_fu8(p_fu8(mk_fu8(k))));
        print("if12 ", h_if12(p_if12(mk_if12(k))), " f3 ", h_f3(p_f3(mk_f3(k))),
              " df12 ", h_df12(p_df12(mk_df12(k))));
        print("u8i64 ", h_u8i64(p_u8i64(mk_u8i64(k))), " i64u8 ", h_i64u8(mk_i64u8(k)),
              " di ", h_di(p_di(mk_di(k))), " id ", h_id(p_id(mk_id(k))), " d2 ", h_d2(mk_d2(k)));
        print("odd16 ", h_odd16(p_odd16(mk_odd16(k))), " s17 ", h_s17(p_s17(mk_s17(k))),
              " s24 ", h_s24(mk_s24(k)), " s33 ", h_s33(p_s33(mk_s33(k))));
        print("arr ", h_arr(mk_arr(k)), " nest ", h_nest(p_nest(mk_nest(k))),
              " nest8 ", h_nest8(p_nest8(mk_nest8(k))));
        print("small ", small(ret_i8(k), (250 + k) as u8, (-30000 - k) as i16, ret_u16(k), k == 1,
                             0.5), " ", ret_i8(k), " ", ret_u16(k));
        print("many_int ", many_int(1, 2, 3, 4, 5 + k, mk_id(k), 6, mk_i64u8(k), 7 - k));
        print("many_sse ", many_sse(1.0, 2.0, 3.0, 4.0, 5.0, 6.0 + k, mk_d2(k), 7.0,
                                    mk_f2(k), 8.5, 9.0));
        print("many_mix ", many_mix(mk_di(k), mk_id(k), mk_di(k + 1), mk_s17(k), mk_di(k + 2),
                                    mk_odd16(k), mk_if12(k), mk_b3(k), mk_f3(k), mk_di(k + 3),
                                    (200 + k) as u8, -1.25));
        let r = many_ret(1, 2, 3, 4, 5, mk_s24(k), 6 + k, mk_di(k));
        print("many_ret ", h_s33(r), " ", r.a, " ", r.c, " ", r.e);
        let q = many_ret2(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8 + k, mk_df12(k));
        print("many_ret2 ", h_di(q), " ", q.a, " ", q.b);
    }
}
