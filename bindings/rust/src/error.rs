// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Error types for loom-fabric.

use std::io;
use thiserror::Error;

/// Error type for loom operations.
#[derive(Error, Debug)]
pub enum Error {
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
    }

    /// Check if this error indicates a timeout.
    pub fn is_timeout(&self) -> bool {
        matches!(self, Error::Timeout)
    }
}

/// Convert a completion event error code to an Error.
pub(crate) fn from_completion_error(code: i32) -> Error {
    match code {
        -11 => Error::WouldBlock, // EAGAIN
        -110 => Error::Timeout,   // ETIMEDOUT
        -22 => Error::InvalidArgument("invalid argument".into()), // EINVAL
        -95 => Error::NotSupported, // EOPNOTSUPP
        code => Error::Fabric(format!("error code {}", code)),
    }
}
