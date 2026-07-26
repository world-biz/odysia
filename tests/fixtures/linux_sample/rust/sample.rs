pub mod sample_module {
    pub struct RustState {
        pub value: u32,
    }

    pub trait KernelThing {
        fn run(&self);
    }

    pub fn rust_helper() -> u32 {
        1
    }
}

macro_rules! sample_macro {
    () => {};
}
