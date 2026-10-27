use gateway_contracts::provider::UpstreamProvider;
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub enum ExtrasFamily {
    ImageGenerations,
    ImageEdits,
    Embeddings,
    EmbeddingJobCreate,
    EmbeddingJobPoll,
    SystemOne,
    Decisions,
}
impl ExtrasFamily {
    pub fn provider(self) -> UpstreamProvider {
        match self {
            Self::ImageGenerations | Self::ImageEdits => UpstreamProvider::ChatgptCodex,
            Self::SystemOne | Self::Decisions => UpstreamProvider::Openrouter,
            _ => UpstreamProvider::Voyage,
        }
    }
    pub fn child_id(self, parent: &str, index: u32) -> String {
        match self {
            Self::ImageGenerations | Self::ImageEdits if index > 0 => format!(
                "{parent}:image:{}:{}",
                if self == Self::ImageGenerations {
                    "generations"
                } else {
                    "edits"
                },
                index + 1
            ),
            _ => parent.into(),
        }
    }
}
