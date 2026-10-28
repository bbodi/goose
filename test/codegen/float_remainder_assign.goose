// Compound assignment on floats: %= beside the four arithmetic ones.
fn remainders() {
    var f = 7.5;
    f %= 2.0;
    print(f);
    f += 1.25;
    f *= 4.0;
    f /= 2.0;
    f -= 0.5;
    print(f);
    var g: f32 = 9.5;
    g %= 4.0;
    print(g);
}

fn main() {
    remainders();
}
