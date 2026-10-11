// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Error types for loom-fabric.

use std::io;
use thiserror::Error;

/// Error type for loom operations.
#[derive(Error, Debug)]
pub enum Error {
    /// Managed operations must retire before the endpoint can be replaced.
    #[error("Endpoint still has pending managed operations")]
    PendingOperations,
    /// Endpoint-bound registrations must be released before native close.
    #[error("Endpoint still has bound memory registrations")]
    BoundRegistrations,
    /// Endpoint close failed; native resources remain owned and may be retried.
    #[error("Fabric endpoint close failed with code {code}")]
    Close {
        /// The negative libfabric error code.
        code: i32,
    },
    /// The provider rejected a submission; no operation was accepted.
    #[error("Fabric submission rejected with code {code}")]
    Submission {
        /// The negative libfabric error code, preserved without message parsing.
        code: i32,
    },
    /// The queue failed without an attributable operation completion.
    #[error("Fabric completion queue failed with code {code}")]
    Queue {
        /// The negative libfabric error code.
        code: i32,
    },
    /// An operation failed after submission.
    #[error("Fabric completion {} failed with code {}", .event.context, .event.error.unwrap_or_default())]
    Completion {
        /// The complete failed event, including owned provider diagnostics.
        event: Box<crate::CompletionEvent>,
    },
    /// A fabric operation failed.
    #[error("Fabric error: {0}")]
    Fabric(String),

    /// The operation would block (try again).
    #[error("Operation would block")]
    WouldBlock,

    /// Timeout expired.
    #[error("Operation timed out")]
    Timeout,

    /// Invalid argument.
    #[error("Invalid argument: {0}")]
    InvalidArgument(String),

    /// Resource not available.
    #[error("Resource not available")]
    Unavailable,

    /// Operation not supported.
    #[error("Operation not supported")]
    NotSupported,

    /// I/O error (from tokio/async operations).
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),

    /// CXX bridge error.
    #[error("FFI error: {0}")]
    Cxx(#[from] cxx::Exception),
}

/// Result type for loom operations.
pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    /// Create a fabric error from a message.
    pub fn fabric<S: Into<String>>(msg: S) -> Self {
        Error::Fabric(msg.into())
    }

    /// Check if this error indicates the operation would block.
    pub fn is_would_block(&self) -> bool {
        matches!(self, Error::WouldBlock)
            || matches!(self, Error::Submission { code } if *code == crate::ffi::ffi::error_again())
    }

    /// Check if this error indicates a timeout.
    pub fn is_timeout(&self) -> bool {
        matches!(self, Error::Timeout)
            || matches!(self, Error::Submission { code } if *code == crate::ffi::ffi::error_timeout())
    }
}

pub(crate) fn from_submission_status(code: i32) -> Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(Error::Submission { code })
    }
}

/// A failed wait does not establish completion of any accepted operation.
pub(crate) fn from_queue_status(code: i32) -> Error {
    if code == crate::ffi::ffi::error_again() {
        Error::WouldBlock
    } else if code == crate::ffi::ffi::error_timeout() {
        Error::Timeout
    } else {
        Error::Queue { code }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejected_submissions_retain_numeric_status() {
        assert!(from_submission_status(0).is_ok());
        for code in [-5, -22, -256, -1_234_567] {
            let error = from_submission_status(code).unwrap_err();
            assert!(matches!(error, Error::Submission { code: actual } if actual == code));
            assert!(!error.is_would_block());
            assert!(!error.is_timeout());
        }
    }

    #[test]
    fn retry_classification_uses_target_libfabric_constants() {
        let again = crate::ffi::ffi::error_again();
        let timeout = crate::ffi::ffi::error_timeout();
        let error = from_submission_status(again).unwrap_err();
        assert!(error.is_would_block());
        assert!(!error.is_timeout());
        assert!(matches!(error, Error::Submission { code } if code == again));
        let error = from_submission_status(timeout).unwrap_err();
        assert!(error.is_timeout());
        assert!(!error.is_would_block());
        assert!(matches!(error, Error::Submission { code } if code == timeout));
        assert!(Error::WouldBlock.is_would_block());
        assert!(Error::Timeout.is_timeout());
        // An error after acceptance must not invite a duplicate submission.
        assert!(!Error::Completion {
            event: Box::new(crate::CompletionEvent {
                context: 1,
                error: Some(again),
                ..Default::default()
            })
        }
        .is_would_block());
    }
}
