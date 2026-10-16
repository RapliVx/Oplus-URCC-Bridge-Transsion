use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn generate(source: &Path, out_file: &Path, label: &str) {
    rsbinder_aidl::Builder::new()
        .source(source)
        .output(out_file)
        .generate()
        .unwrap_or_else(|error| panic!("failed to generate AIDL bindings for {label}: {error}"));

    let generated = fs::read_to_string(out_file)
        .unwrap_or_else(|error| panic!("failed to read generated {label} bindings: {error}"));

    let mut version_code_patched = false;
    let mut hash_code_patched = false;
    let generated = generated
        .lines()
        .map(|line| {
            if line.contains("const r#getInterfaceVersion:") {
                version_code_patched = true;
                "                        pub(crate) const r#getInterfaceVersion: rsbinder::TransactionCode = 16777215;".to_string()
            } else if line.contains("const r#getInterfaceHash:") {
                hash_code_patched = true;
                "                        pub(crate) const r#getInterfaceHash: rsbinder::TransactionCode = 16777214;".to_string()
            } else if line.contains("#![allow(non_upper_case_globals, non_snake_case, dead_code)]") {
                line.replace("dead_code", "dead_code, non_camel_case_types")
            } else {
                line.to_string()
            }
        })
        .collect::<Vec<_>>()
        .join("\n");

    assert!(
        version_code_patched && hash_code_patched,
        "generated {label} bindings no longer contain interface metadata transaction constants"
    );
    fs::write(out_file, generated).unwrap_or_else(|error| {
        panic!("failed to patch generated {label} transaction codes: {error}")
    });
}

fn main() {
    println!("cargo:rerun-if-changed=aidl");
    let out_dir = env::var("OUT_DIR").unwrap();
    generate(
        &PathBuf::from("aidl"),
        &PathBuf::from(&out_dir).join("urcc.rs"),
        "IUrcc",
    );
}
