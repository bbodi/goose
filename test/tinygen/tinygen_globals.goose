// The tinygen backend: globals, read and written from tinygen code through
// an accessor giving each one's address: the C half's, or built whole with
// no C half, the object's own storage and its gs_init_globals. A const
// `let` with a compile-time initializer is static data, which the object
// holds a copy of. A global whose initializer calls a function runs that
// function (compiled by tinygen) from the globals' initialization.

struct Vec2 { x: f64, y: f64 }

let LIMIT: const i64 = 1000;
let PRIMES: const i32[6] = [2, 3, 5, 7, 11, 13];
let ORIGIN: const Vec2 = Vec2 { x: 0.5, y: -1.25 };
let TAG: const u8[3] = "abc";
let SCALE = 3;
var counter: i64 = 0;
var last = Vec2 { x: 0.0, y: 0.0 };
var hist: u16[4];
var seed = start_seed(7);
let qa = divmod(LIMIT + 3, 7);

fn start_seed(n: i64) -> i64 { n * 11 + SCALE }

fn divmod(a: i64, b: i64) -> i64, i64 { return a / b, a % b; }

fn bump(by: i64) -> i64 {
    counter += by;
    counter
}

fn next() -> i64 {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    hist[seed % 4]++;
    seed
}

fn move_to(v: Vec2) {
    last.x = v.x + ORIGIN.x;
    last.y = v.y + ORIGIN.y;
}

fn sum_primes() -> i64 {
    var s = 0;
    for p in PRIMES { s += p; }
    s
}

fn main() {
    print(seed, " ", qa);
    bump(5);
    print(bump(LIMIT));
    for i in 0..10 { next(); }
    print(seed, " ", hist[0], " ", hist[1], " ", hist[2], " ", hist[3]);
    move_to(Vec2 { x: 2.0, y: 3.0 });
    print(last.x, " ", last.y);
    print(sum_primes(), " ", PRIMES[5] * SCALE, " ", TAG[1]);
    counter = counter * 2;
    print(counter);
}
