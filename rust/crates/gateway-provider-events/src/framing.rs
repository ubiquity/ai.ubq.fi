use gateway_provider_source::extras::malformed;

use gateway_source::error::GatewayError;

#[derive(Clone, Debug)]
pub struct SseFrame {
    pub event: Option<String>,
    pub data: String,
}

#[derive(Clone, Debug)]
pub struct SseParser {
    pending: Vec<u8>,
    limit: usize,
}

impl SseParser {
    pub fn new(limit: usize) -> Self {
        Self {
            pending: vec![],
            limit,
        }
    }
    pub fn push(&mut self, bytes: &[u8]) -> Result<Vec<SseFrame>, GatewayError> {
        self.pending.extend_from_slice(bytes);
        let mut frames = vec![];
        while let Some((end, width)) = boundary(&self.pending) {
            if end > self.limit {
                return Err(malformed("Upstream SSE frame exceeded its bound."));
            }
            let raw = self.pending.drain(..end + width).collect::<Vec<_>>();
            let text = std::str::from_utf8(&raw[..end])
                .map_err(|_| malformed("Upstream SSE contains invalid UTF-8."))?;
            if let Some(frame) = parse_frame(text) {
                frames.push(frame);
            }
        }
        if self.pending.len() > self.limit {
            return Err(malformed("Upstream SSE frame exceeded its bound."));
        }
        Ok(frames)
    }
    pub fn finish(&mut self) -> Result<Vec<SseFrame>, GatewayError> {
        if self.pending.is_empty() {
            return Ok(vec![]);
        }
        let bytes = std::mem::take(&mut self.pending);
        let text = std::str::from_utf8(&bytes)
            .map_err(|_| malformed("Upstream SSE contains invalid UTF-8."))?;
        Ok(parse_frame(text).into_iter().collect())
    }
}

fn boundary(bytes: &[u8]) -> Option<(usize, usize)> {
    for i in 0..bytes.len() {
        let Some(first) = line_ending(bytes, i) else {
            continue;
        };
        if let Some(second) = line_ending(bytes, i + first) {
            return Some((i, first + second));
        }
    }
    None
}

fn line_ending(bytes: &[u8], index: usize) -> Option<usize> {
    match bytes.get(index) {
        Some(b'\r') if bytes.get(index + 1) == Some(&b'\n') => Some(2),
        Some(b'\r' | b'\n') => Some(1),
        _ => None,
    }
}

fn parse_frame(text: &str) -> Option<SseFrame> {
    let normalized = text.replace("\r\n", "\n").replace('\r', "\n");
    let mut event = None;
    let mut data = vec![];
    for line in normalized.lines() {
        if line.starts_with(':') {
            continue;
        }
        let (key, value) = line.split_once(':').unwrap_or((line, ""));
        let value = value.strip_prefix(' ').unwrap_or(value);
        match key {
            "data" => data.push(value),
            "event" => event = Some(value.to_owned()),
            _ => (),
        }
    }
    (!data.is_empty()).then(|| SseFrame {
        event,
        data: data.join("\n"),
    })
}
