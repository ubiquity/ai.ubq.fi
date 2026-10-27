//! Dev-only original fixtures, recipes and synthetic host counters.
macro_rules! text {($($s:expr),*$(,)?)=>{{let mut units=Vec::new();$(gateway_compaction_source::text::Part::append(&$s,&mut units);)*gateway_compaction_source::text::Text::new(units)}};}
use gateway_compaction_source::{
    model::{Message, Role, ToolResult, ToolUse},
    state,
    text::{self, Text},
    types::CompactionHost,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::Deserialize;
use std::{
    cell::{Cell, RefCell},
    future::Future,
    task::{Context, Poll, Waker},
};
pub fn run<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        match future.as_mut().poll(&mut cx) {
            Poll::Ready(value) => return value,
            Poll::Pending => std::thread::yield_now(),
        }
    }
}
pub fn fixture() -> SourceJson {
    SourceJson::parse(include_str!("../tests/fixtures/source-v1.json"))
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}
pub fn cases(name: &str) -> Vec<SourceJson> {
    text::member(&fixture(), name)
        .unwrap_or_else(|| panic!("missing source fixture value"))
        .array_items()
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}
pub fn decode<T: serde::de::DeserializeOwned>(value: &SourceJson) -> T {
    value
        .deserialize()
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}
pub fn equal(actual: &SourceJson, expected: &SourceJson) {
    let a =
        text::canonical(actual).unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
    let b = text::canonical(expected)
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
    if a == b {
        return;
    }
    let i = a
        .as_bytes()
        .iter()
        .zip(b.as_bytes())
        .position(|(a, b)| a != b)
        .unwrap_or(a.as_bytes().len().min(b.as_bytes().len()));
    let snippet = |s: &str| {
        let mut lo = i.saturating_sub(80).min(s.len());
        while !s.is_char_boundary(lo) {
            lo -= 1;
        }
        let mut hi = (i + 200).min(s.len());
        while !s.is_char_boundary(hi) {
            hi -= 1;
        }
        s[lo..hi].to_owned()
    };
    panic!(
        "source JSON mismatch at byte {i}; actual={} expected={}",
        snippet(a.as_str()),
        snippet(b.as_str())
    );
}
pub fn field(value: &SourceJson, name: &str) -> SourceJson {
    text::member(value, name).unwrap_or_else(|| panic!("missing source fixture value"))
}
pub fn string(value: &SourceJson) -> String {
    text::string(value)
        .unwrap_or_else(|| panic!("missing source fixture value"))
        .try_utf8()
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}
#[derive(Deserialize)]
pub struct Recipe {
    pub count: usize,
    pub text: usize,
    pub input: usize,
    #[serde(default)]
    pub notes: bool,
}
pub fn recipe(r: &Recipe) -> Vec<Message> {
    let mut out = vec![Message {
        role: Role::User,
        text: "Goal: preserve current state.".into(),
        tool_uses: Vec::new(),
        tool_results: None,
    }];
    for i in 0..r.count {
        let input = text::object(vec![(
            "cmd",
            text::quoted(&Text::from(format!(
                "command {i} {}",
                "script ".repeat(r.input)
            )))
            .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
        )])
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
        out.push(Message {
            role: Role::Assistant,
            text: if r.text == 0 {
                Text::default()
            } else {
                format!("older alpha bravo {i} ").repeat(r.text).into()
            },
            tool_uses: vec![ToolUse {
                tool_use_id: text!("call_", i),
                tool: "exec_command".into(),
                input: std::rc::Rc::new(input),
                text: None,
                is_error: None,
            }],
            tool_results: None,
        });
        out.push(Message {
            role: Role::User,
            text: Text::default(),
            tool_uses: Vec::new(),
            tool_results: Some(vec![ToolResult {
                tool_use_id: text!("call_", i),
                text: format!("result {i} ").repeat(50).into(),
                is_error: None,
            }]),
        });
    }
    if r.notes {
        out.insert(
            out.len() - 2,
            Message {
                role: Role::User,
                text: text!("Older plain note ", r.count),
                tool_uses: Vec::new(),
                tool_results: None,
            },
        );
    }
    out.push(Message {
        role: Role::User,
        text: "Final goal.".into(),
        tool_uses: Vec::new(),
        tool_results: None,
    });
    out
}
pub struct Host {
    pub mode: String,
    pub residue: String,
    pub asks: RefCell<Vec<SourceJson>>,
    pub residue_calls: RefCell<Vec<SourceJson>>,
    pub oversized: usize,
    pub cancel: Cell<bool>,
    pub cancel_after_ask: bool,
    pub cancel_after_residue: bool,
}
impl Host {
    pub fn new(mode: &str) -> Self {
        Self {
            mode: mode.into(),
            residue: "failure".into(),
            asks: RefCell::new(Vec::new()),
            residue_calls: RefCell::new(Vec::new()),
            oversized: 0,
            cancel: Cell::new(false),
            cancel_after_ask: false,
            cancel_after_residue: false,
        }
    }
}
impl CompactionHost for Host {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(1_700_000_000_000)
    }
    fn random_uuid(&self) -> Result<String, GatewayError> {
        Ok("00000000-0000-4000-8000-000000000001".into())
    }
    fn cancelled(&self) -> bool {
        self.cancel.get()
    }
    async fn ask_jev(
        &self,
        state: &SourceJson,
        questions: &SourceJson,
        timeout: u32,
    ) -> Result<SourceJson, GatewayError> {
        assert_eq!(timeout, 30_000);
        self.asks.borrow_mut().push(
            text::object(vec![
                ("state", state.clone()),
                ("questions", questions.clone()),
            ])
            .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
        );
        if self.cancel_after_ask {
            self.cancel.set(true);
        }
        if self.mode == "throw" {
            return Err(GatewayError::new(
                500,
                "private provider text",
                Some("controlled"),
            ));
        }
        let mut answers = Vec::new();
        for (name, _) in
            text::entries(questions).unwrap_or_else(|| panic!("missing source fixture value"))
        {
            let name_text = name
                .try_utf8()
                .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
            if self.mode == "partial" && name_text.starts_with("result_") {
                continue;
            }
            let id = name_text
                .split("_t")
                .nth(1)
                .unwrap_or_else(|| panic!("missing source fixture value"))
                .parse::<usize>()
                .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
            let mut value = 0.1;
            if self.mode == "mixed" {
                value = if id == 3 || id == 2 && name_text.starts_with("call_") {
                    0.9
                } else {
                    0.1
                };
            }
            if self.mode == "keep" {
                value = 1.0;
            }
            if self.mode == "equal" {
                value = 0.7;
            }
            if self.mode == "ranked" {
                value = if name_text.starts_with("call_") {
                    0.9
                } else {
                    [0.6, 0.7, 0.8, 0.9, 0.95][id - 1]
                };
            }
            if self.mode == "outside" {
                value = match (id, name_text.starts_with("call_")) {
                    (1, true) | (2, _) => 2.0,
                    _ => -1.0,
                };
            }
            let number = if self.mode == "nonfinite" && name_text == "result_t1" {
                text::raw("\"0.1\"")
                    .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
            } else {
                state::json(&value)
                    .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
            };
            answers.push((
                name,
                text::object(vec![("noul", number)])
                    .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
            ));
        }
        Ok(text::object(vec![(
            "answers",
            text::object_keys(answers)
                .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
        )])
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")))
    }
    async fn rewrite_residue(
        &self,
        body: &SourceJson,
        timeout: u32,
    ) -> Result<SourceJson, GatewayError> {
        assert!(timeout == 20_000 || timeout == 1000);
        self.residue_calls.borrow_mut().push(body.clone());
        if self.cancel_after_residue {
            self.cancel.set(true);
        }
        let n = self.residue_calls.borrow().len();
        if self.residue == "failure"
            || self.residue == "fail"
            || (self.residue == "partial" && n == 2)
        {
            return Err(GatewayError::new(
                500,
                "controlled residue failure",
                Some("controlled"),
            ));
        }
        let content = if self.residue == "empty" {
            "  ".into()
        } else if self.residue == "oversized" {
            "x".repeat(self.oversized)
        } else {
            "SLIM_MODEL_MARKER compact text.".into()
        };
        Ok(SourceJson::from_serializable(
            &serde_json::json!({"choices":[{"message":{"content":content}}]}),
        )
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")))
    }
}
pub fn fitting_input(recipe: &SourceJson, pattern: &str, prompt: &str) -> SourceJson {
    use serde_json::{Value, json};
    let message = |value: String| json!({"type":"message","role":"user","content":[{"type":"input_text","text":value}]});
    let call = |id: &str, command: &str| json!({"type":"function_call","call_id":id,"name":"shell","arguments":json!({"command":command}).to_string()});
    let output = |id: &str, value: String| json!({"type":"function_call_output","call_id":id,"output":value});
    let large = |n: usize| pattern.repeat(n.div_ceil(pattern.len()))[..n].to_owned();
    let kind = string(&field(recipe, "kind"));
    let mut input: Vec<Value> = if kind == "fit" {
        let mut values = vec![message("Goal: keep the release evidence.".into())];
        for (key, letter) in [('a', 'A'), ('b', 'B'), ('c', 'C'), ('d', 'D'), ('e', 'E')] {
            let id = format!("old_{key}");
            values.push(call(&id, &format!("cat /tmp/{key}")));
            values.push(output(&id, letter.to_string().repeat(360_000)));
        }
        values
    } else if kind == "oversized" {
        let mut values = vec![
            message(format!("Goal: {}", "x".repeat(1_501_000))),
            call("old", "echo old"),
            output("old", "RESULT_OBSOLETE ".repeat(40)),
        ];
        for recent in [
            "recent one",
            "recent two",
            "recent three",
            "recent four",
            "recent five",
            "recent six",
        ] {
            values.push(message(recent.into()));
        }
        values
    } else {
        let mut values = vec![
            message("Goal: finish the large-session check.".into()),
            call("big", "cat /tmp/big.log"),
            output("big", large(decode(&field(recipe, "result")))),
        ];
        for _ in 0..8 {
            values.push(message(large(decode::<usize>(&field(recipe, "text")) / 8)));
        }
        values.push(message("Final goal: keep the exact state.".into()));
        values
    };
    input.push(message(prompt.into()));
    state::json(&serde_json::json!({"model":"fixture-model","input":input}))
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}

pub struct PendingHost {
    pub base: Host,
    pub active: Cell<usize>,
    pub peak: Cell<usize>,
}
impl PendingHost {
    pub fn new() -> Self {
        let mut base = Host::new("drop");
        base.residue = "success".into();
        Self {
            base,
            active: Cell::new(0),
            peak: Cell::new(0),
        }
    }
    async fn pending_once(&self) {
        struct Active<'a>(&'a Cell<usize>);
        impl Drop for Active<'_> {
            fn drop(&mut self) {
                self.0.set(self.0.get() - 1);
            }
        }
        self.active.set(self.active.get() + 1);
        self.peak.set(self.peak.get().max(self.active.get()));
        let _active = Active(&self.active);
        let mut pending = true;
        std::future::poll_fn(|cx| {
            if std::mem::take(&mut pending) {
                cx.waker().wake_by_ref();
                Poll::Pending
            } else {
                Poll::Ready(())
            }
        })
        .await;
    }
}
impl CompactionHost for PendingHost {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        self.base.now_ms()
    }
    fn random_uuid(&self) -> Result<String, GatewayError> {
        self.base.random_uuid()
    }
    fn cancelled(&self) -> bool {
        self.base.cancelled()
    }
    async fn ask_jev(
        &self,
        state: &SourceJson,
        questions: &SourceJson,
        timeout: u32,
    ) -> Result<SourceJson, GatewayError> {
        self.pending_once().await;
        self.base.ask_jev(state, questions, timeout).await
    }
    async fn rewrite_residue(
        &self,
        body: &SourceJson,
        timeout: u32,
    ) -> Result<SourceJson, GatewayError> {
        self.pending_once().await;
        self.base.rewrite_residue(body, timeout).await
    }
}
pub fn fitting_fixture() -> SourceJson {
    SourceJson::parse(include_str!("../tests/fixtures/fitting-v1.json"))
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}
pub fn state_supplemental_fixture() -> SourceJson {
    SourceJson::parse(include_str!("../tests/fixtures/state-supplemental-v1.json"))
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"))
}

impl Default for PendingHost {
    fn default() -> Self {
        Self::new()
    }
}
