use std::collections::HashMap;
use std::sync::{Arc, Mutex, mpsc};
use std::thread;
fn main() {
    let m = Arc::new(Mutex::new(0u64));
    let hs: Vec<_> = (0..4).map(|i| { let m = m.clone(); thread::spawn(move || { for _ in 0..1000 { *m.lock().unwrap() += i; } }) }).collect();
    for h in hs { h.join().unwrap(); }
    println!("sum {}", *m.lock().unwrap());
    let (tx, rx) = mpsc::channel(); thread::spawn(move || { for i in 0..5 { tx.send(i * i).unwrap(); } });
    println!("chan {:?}", rx.iter().collect::<Vec<_>>());
    let mut h = HashMap::new(); for w in "a b a c b a".split(' ') { *h.entry(w).or_insert(0) += 1; } let mut v: Vec<_> = h.into_iter().collect(); v.sort(); println!("{:?}", v);
    let r = std::panic::catch_unwind(|| { let v: Vec<i32> = vec![]; v[3] }); println!("panic caught {}", r.is_err());
    println!("{}", std::fs::read_to_string("/etc/hosts").map(|s| s.len() > 0).unwrap_or(false));
    println!("{:?}", std::process::Command::new("echo").arg("hi").output().map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string()));
    println!("f64 {:.4} {}", (2.0f64).sqrt(), u128::MAX);
}
