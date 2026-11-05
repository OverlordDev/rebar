use std::panic;
struct D(&'static str);
impl Drop for D { fn drop(&mut self) { println!("drop {}", self.0); } }
fn inner(n: u32) -> u32 { let _a = D("inner-a"); if n == 0 { panic!("boom {}", n); } let _b = D("inner-b"); inner(n - 1) + 1 }
fn main() {
    panic::set_hook(Box::new(|_| {}));
    let r = panic::catch_unwind(|| { let _x = D("outer"); inner(2) });
    println!("r err {}", r.is_err());
    if let Err(e) = r { println!("msg {:?}", e.downcast_ref::<String>()); }
    let t = std::thread::spawn(|| { let _t = D("thread"); let v: Option<u8> = None; v.unwrap() });
    println!("join err {}", t.join().is_err());
    let r2 = panic::catch_unwind(|| { let r = panic::catch_unwind(|| { let _y = D("nested"); panic!("a"); }); println!("inner caught {}", r.is_err()); panic::resume_unwind(Box::new(7i32)); });
    println!("resumed {:?}", r2.err().and_then(|e| e.downcast_ref::<i32>().copied()));
    let res: Result<u8, _> = "300".parse::<u8>(); println!("{:?}", res.is_err());
    let _z = D("end");
}
