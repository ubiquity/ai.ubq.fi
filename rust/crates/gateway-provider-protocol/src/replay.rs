use gateway_core::source_json::SourceJson;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReplayTransport {
    Messages,
    Chat,
}


#[derive(Clone, PartialEq)]
pub enum ReplayEntry {
    Original(SourceJson),
    NativeAssistant {
        transport: ReplayTransport,
        payload: SourceJson,
    },
}



impl std::fmt::Debug for ReplayEntry {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Original(_) => f.write_str("Original([private conversation item])"),
            Self::NativeAssistant { transport, .. } => f
                .debug_struct("NativeAssistant")
                .field("transport", transport)
                .field("payload", &"[private signed blocks]")
                .finish(),
        }
    }
}
