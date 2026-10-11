// A division by zero in a function the tinygen backend compiles aborts with
// the runtime's message and the Goose line, as from the C backend: in every
// build, since the check is not optional (§6.2).

fn ratio(a: i32, b: i32) -> i32 { a / b }

fn main() {
    var d: i32 = 3;
    print(ratio(12, d));
    d -= 3;
    print(ratio(12, d));
    print("not reached");
}
