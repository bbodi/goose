// from_bytes<T[>..]> over a bytes_of view of another array: the view's bytes
// carry no framing prefix, so only to_bytes' image verifies.
struct Pt { x: i64, y: i64 }

fn round_trips() {
    var ps: Pt[>..] = [Pt { x: 1, y: 2 }, Pt { x: 3, y: 4 }];
    let back, ok = from_bytes<Pt[>..]>(to_bytes(ps));
    print(ok, " ", back.len, " ", back[1].y);
    let view, ok2 = from_bytes<Pt[>..]>(bytes_of(ps));
    print(ok2, " ", view.len);
}

fn main() {
    round_trips();
}
