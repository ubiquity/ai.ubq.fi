//! Source-compatible compaction render definitions.
macro_rules! text {($($s:expr),*$(,)?)=>{{let mut units=Vec::new();$(gateway_compaction_source::text::Part::append(&$s,&mut units);)*gateway_compaction_source::text::Text::new(units)}};}
pub mod hybrid;
pub mod response;
#[cfg(test)]
mod tests;
pub mod transcript;
