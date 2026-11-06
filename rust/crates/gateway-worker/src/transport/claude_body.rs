//! Source Claude error capture drains a clone and retains JS UTF-16 code units.
use futures_util::StreamExt;
use worker::{Response, ResponseBody};

use super::outbound::Cancellation;

pub const PREFIX_UNITS: usize = 4096;

#[derive(Default)]
pub struct ErrorCapture {
    pub prefix: Option<Vec<u16>>,
    pub complete: bool,
}

/// Header observation belongs before this call. The source request lifetime is
/// the only deadline; a full retained prefix does not end the clone's drain.
pub async fn capture(response: &mut Response, cancellation: &Cancellation) -> ErrorCapture {
    let Ok(mut cloned) = response.cloned() else {
        return ErrorCapture::default();
    };
    if matches!(cloned.body(), ResponseBody::Empty) {
        return ErrorCapture {
            prefix: Some(Vec::new()),
            complete: true,
        };
    }
    let Ok(child) = cancellation.child() else {
        return ErrorCapture::default();
    };
    let Ok(mut source) = cloned.stream() else {
        return ErrorCapture::default();
    };
    let mut prefix = Utf16Prefix::default();
    let complete = loop {
        let next = std::future::poll_fn(|context| {
            child.register_waker(context.waker());
            if child.signal().aborted() {
                std::task::Poll::Ready(None)
            } else {
                source.poll_next_unpin(context)
            }
        })
        .await;
        match next {
            None => break !child.signal().aborted(),
            Some(Err(_)) => break false,
            Some(Ok(chunk)) => prefix.push(&chunk),
        }
    };
    drop(source);
    if complete {
        prefix.finish();
    }
    ErrorCapture {
        prefix: Some(prefix.units),
        complete,
    }
}

#[derive(Default)]
pub struct Utf16Prefix {
    pub units: Vec<u16>,
    pending: Vec<u8>,
    started: bool,
}

impl Utf16Prefix {
    fn text(&mut self, text: &str) {
        for character in text.chars() {
            if !self.started {
                self.started = true;
                if character == '\u{feff}' {
                    continue;
                }
            }
            let mut encoded = [0; 2];
            for unit in character.encode_utf16(&mut encoded) {
                if self.units.len() == PREFIX_UNITS {
                    return;
                }
                self.units.push(*unit);
            }
        }
    }

    pub fn push(&mut self, chunk: &[u8]) {
        // Bound only decoder scratch, while the caller still drains every
        // network chunk even after the retained JS prefix is full.
        for part in chunk.chunks(PREFIX_UNITS) {
            if self.units.len() == PREFIX_UNITS {
                return;
            }
            self.push_piece(part);
        }
    }

    fn push_piece(&mut self, chunk: &[u8]) {
        if self.units.len() == PREFIX_UNITS {
            return;
        }
        let mut joined = std::mem::take(&mut self.pending);
        joined.extend_from_slice(chunk);
        let mut remaining = joined.as_slice();
        loop {
            match std::str::from_utf8(remaining) {
                Ok(text) => {
                    self.text(text);
                    return;
                }
                Err(error) => {
                    let (valid, tail) = remaining.split_at(error.valid_up_to());
                    self.text(&String::from_utf8_lossy(valid));
                    if self.units.len() == PREFIX_UNITS {
                        return;
                    }
                    match error.error_len() {
                        Some(length) => {
                            self.text("\u{fffd}");
                            remaining = &tail[length..];
                        }
                        None => {
                            self.pending.extend_from_slice(tail);
                            return;
                        }
                    }
                }
            }
        }
    }

    pub fn finish(&mut self) {
        if !self.pending.is_empty() {
            self.text("\u{fffd}");
            self.pending.clear();
        }
    }
}
