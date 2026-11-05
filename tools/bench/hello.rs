fn main(){ let v: Vec<u64> = (0..1000).map(|x| x*x).collect(); println!("{}", v.iter().sum::<u64>()); }
