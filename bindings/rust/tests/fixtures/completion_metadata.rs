// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
// Compiled as a library unit-test module so the production FFI stays private.
#![cfg(all(test, feature = "test_fixtures"))]
use crate::{ffi, CompletionErrorInfo, CompletionEvent, Error};
use ffi::ffi::TestCompletionKind as Kind;

fn read(
    kind: Kind,
    waiting: bool,
    sources: bool,
    source: u64,
    buffer: bool,
    zero: bool,
) -> ffi::CqReadResult {
    ffi::ffi::test_completion_read(
        kind,
        &ffi::CqAttributes {
            size: 16,
            wait_obj: waiting,
            source_addresses: sources,
        },
        0x1234,
        source,
        buffer,
        zero,
    )
    .unwrap()
}

#[test]
fn native_completion_metadata_survives_bridge_and_queue_teardown() {
    for waiting in [false, true] {
        for kind in [Kind::Success, Kind::Error] {
            for sources in [false, true] {
                for source in [0, 0xfedcba9876543210, u64::MAX] {
                    for buffer in [false, true] {
                        let result = read(kind, waiting, sources, source, buffer, false);
                        assert_eq!(result.kind, ffi::CqReadKind::Event);
                        let event = CompletionEvent::from_raw(result.event);
                        assert_eq!(event.context, 0x1234);
                        assert_eq!(event.bytes_transferred, 5);
                        assert_ne!(event.flags, 0);
                        assert_eq!(event.data, 0xfedcba9876543210);
                        assert_eq!(event.tag, 0xabcdef0123456789);
                        assert_eq!(event.buffer, buffer.then_some(0x4321));
                        assert_eq!(
                            event.source,
                            (sources && source != u64::MAX).then_some(source)
                        );
                        if kind == Kind::Error {
                            assert!(event.error.is_some_and(|code| code < 0));
                            assert_eq!(
                                event.error_info,
                                Some(CompletionErrorInfo {
                                    provider_error: -37,
                                    data: vec![0x52, 0, 0xff, 0x79],
                                    overflow_length: 17,
                                })
                            );
                            let expected = event.clone();
                            let Error::Completion { event } = event.into_result().unwrap_err()
                            else {
                                panic!("native operation error was reclassified");
                            };
                            assert_eq!(*event, expected);
                        } else {
                            assert_eq!(event.error, None);
                            assert_eq!(event.error_info, None);
                            assert!(event.into_result().is_ok());
                        }
                    }
                }
            }
        }
    }
}

#[test]
fn zero_native_error_fields_are_available_values() {
    for waiting in [false, true] {
        let result = read(Kind::Error, waiting, true, 0, false, true);
        let event = CompletionEvent::from_raw(result.event);
        assert_eq!(event.error_info, Some(CompletionErrorInfo::default()));
        assert_eq!(event.source, Some(0));
        assert_eq!(event.buffer, None);
    }
}

#[test]
fn queue_failures_and_empty_reads_do_not_invent_native_metadata() {
    for waiting in [false, true] {
        for kind in [Kind::QueueFailure, Kind::Empty] {
            let result = read(kind, waiting, true, 0, true, false);
            if waiting {
                assert_eq!(result.kind, ffi::CqReadKind::Failure);
                assert_ne!(result.error_code, 0);
            } else if kind == Kind::Empty {
                assert_eq!(result.kind, ffi::CqReadKind::Empty);
            } else {
                assert_eq!(result.kind, ffi::CqReadKind::Event);
                let event = CompletionEvent::from_raw(result.event);
                assert!(event.error.is_some());
                assert_eq!(event.error_info, None);
                assert_eq!(event.source, None);
                assert_eq!(event.buffer, None);
            }
        }
    }
}
