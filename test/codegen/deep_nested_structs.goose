// A struct nested 34 levels deep, one field each: the value is built, read back
// through every level and printed.
struct S0 { a: i64 }
struct S1 { a: S0 }
struct S2 { a: S1 }
struct S3 { a: S2 }
struct S4 { a: S3 }
struct S5 { a: S4 }
struct S6 { a: S5 }
struct S7 { a: S6 }
struct S8 { a: S7 }
struct S9 { a: S8 }
struct S10 { a: S9 }
struct S11 { a: S10 }
struct S12 { a: S11 }
struct S13 { a: S12 }
struct S14 { a: S13 }
struct S15 { a: S14 }
struct S16 { a: S15 }
struct S17 { a: S16 }
struct S18 { a: S17 }
struct S19 { a: S18 }
struct S20 { a: S19 }
struct S21 { a: S20 }
struct S22 { a: S21 }
struct S23 { a: S22 }
struct S24 { a: S23 }
struct S25 { a: S24 }
struct S26 { a: S25 }
struct S27 { a: S26 }
struct S28 { a: S27 }
struct S29 { a: S28 }
struct S30 { a: S29 }
struct S31 { a: S30 }
struct S32 { a: S31 }
struct S33 { a: S32 }

fn deep() {
    var x = S33 { a: S32 { a: S31 { a: S30 { a: S29 { a: S28 { a: S27 { a: S26 { a: S25 { a: S24 { a: S23 { a: S22 { a: S21 { a: S20 { a: S19 { a: S18 { a: S17 { a: S16 { a: S15 { a: S14 { a: S13 { a: S12 { a: S11 { a: S10 { a: S9 { a: S8 { a: S7 { a: S6 { a: S5 { a: S4 { a: S3 { a: S2 { a: S1 { a: S0 { a: 7 } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } };
    print(x.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a);
    print(x);
}

fn main() {
    deep();
}
