// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use std::{env, fs, path::PathBuf};

fn main() {
    cxx_build::bridge("src/bridge.rs")
        .include("include")
        .std("c++17")
        .compile("local_cxx");
    if let Ok(directory) = env::var("LOCAL_HEADER_DIR") {
        let directory = PathBuf::from(directory);
        fs::create_dir_all(&directory).expect("cannot create local header directory");
        let source = PathBuf::from(env::var("OUT_DIR").unwrap())
            .join("cxxbridge/include/local/src/bridge.rs.h");
        let bytes = fs::read(source).expect("cannot read local CXX header");
        let target = directory.join("local_bridge.h");
        if fs::read(&target).ok().as_deref() != Some(bytes.as_slice()) {
            fs::write(target, bytes).expect("cannot publish local CXX header");
        }
    }
    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=include/callbacks.h");
    println!("cargo:rerun-if-env-changed=LOCAL_HEADER_DIR");
}
