//! Source-compatible compaction source definitions.
macro_rules! text {($($s:expr),*$(,)?)=>{{let mut units=Vec::new();$(crate::text::Part::append(&$s,&mut units);)*crate::text::Text::new(units)}};}
pub mod model;
pub mod state;
#[cfg(test)]
mod tests;
pub mod text;
pub mod types;
