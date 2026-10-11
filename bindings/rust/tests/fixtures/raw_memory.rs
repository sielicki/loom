// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
use crate::ffi;

#[test]
fn scalar_key_exports_reject_raw_and_unavailable_native_keys() {
    for raw in [false, true] {
        for key in [0, 0xabcdef0123456789, u64::MAX] {
            let scalar = ffi::ffi::test_memory_key(raw, key);
            let remote = ffi::ffi::test_memory_export(raw, key);
            if raw || key == u64::MAX {
                assert!(scalar.is_err());
                assert!(remote.is_err());
            } else {
                assert_eq!(scalar.unwrap(), key);
                let remote = remote.unwrap();
                assert_eq!(remote.key, key);
                assert_eq!(remote.addr, 0x1234);
                assert_eq!(remote.length, 64);
            }
        }
    }
}
