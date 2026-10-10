// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0

fn main() {
    let directory = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let config = cbindgen::Config::from_file(directory.join("cbindgen.toml")).unwrap();
    std::fs::create_dir_all(directory.join("include")).unwrap();
    cbindgen::Builder::new()
        .with_crate(&directory)
        .with_config(config)
        .generate()
        .expect("KV C header generation failed")
        .write_to_file(directory.join("include/kv.h"));
    println!("cargo:rerun-if-changed=src/ffi.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
}
