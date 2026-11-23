// Recorded from Bun 1.4.0 running src/models/codex-models.ts: every input (JSON text that
// keeps -0; "__INFINITY__" stands for Infinity in overrides) and JSON.stringify of each result.
inline constexpr std::string_view CODEX_MODELS_DIFFERENTIAL_VECTORS = R"VECTORS({
 "capNegZero": [
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [
   "0.controls.verified_at_ms"
  ],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [
   "0.scope.verified_at_ms"
  ],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  []
 ],
 "lookupSnapshot": "{\"source\":\"s\",\"models\":[{\"slug\":\"\",\"id\":\"e\"},{\"slug\":5,\"id\":\" y \"},{\"model\":\"dup\"},{\"name\":\"dup\"},[],{\"slug\":\"pc\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"source\":\"catalog\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cache_write_tokens\",\"cached_tokens\"]}}]}},{\"slug\":\"pc2\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"implicit\":true,\"source\":\"catalog\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cached_tokens\"]}}]}},{\"slug\":\"pc3\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":\"true\",\"source\":\"catalog\",\"verified_at_ms\":1}}]}},{\"slug\":\"pc4\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"implicit\":false,\"source\":\"catalog\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"]}}]}},{\"slug\":\"pc5\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"other\"},{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"source\":\"inferred\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"modes\":[]}}]}}],\"updated_at_ms\":1}",
 "capVectors": [
  {
   "input": "false",
   "normalized": "false",
   "compact": "false"
  },
  {
   "input": "true",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "null",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "0",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "\"x\"",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "[]",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"a\"}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"a\"}]}",
   "compact": "null"
  },
  {
   "input": "{\"providers\":[{\"id\":\"a\"}],\"version\":1}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"a\"}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"a\"}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"a\"}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":\"1\",\"providers\":[{\"id\":\"a\"}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"a\"}],\"__proto__x\":1}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"a\"}],\"__proto__\":1}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\" a \"},{\"id\":\"a\"}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"\u3000x\u3000\"}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"x\"}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"\u00a0\"}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"\"}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"id\":\"z\"}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"z\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"z\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"verified_at_ms\":5,\"source\":\"inferred\",\"expected_usage_fields\":[\"cache_write_tokens\"],\"key\":false}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":false,\"expected_usage_fields\":[\"cache_write_tokens\"],\"source\":\"inferred\",\"verified_at_ms\":5}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":false,\"expected_usage_fields\":[\"cache_write_tokens\"],\"source\":\"inferred\",\"verified_at_ms\":5}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"in_memory\",\"24h\",\"in_memory\",\" 24h \"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"in_memory\",\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"in_memory\",\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\",\" 30m \"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\",5],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\" implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"explicit\",\"implicit\",\"explicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"explicit\",\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"explicit\",\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cache_write_tokens\",\"cached_tokens\",\"cached_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cache_write_tokens\",\"cached_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cache_write_tokens\",\"cached_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\",\"input_text\",\"input_file\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\",\"input_file\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\",\"input_file\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"chat_completions\":[\"file\",\"image_url\"],\"responses\":[\"bogus\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"chat_completions\":[\"file\",\"image_url\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"chat_completions\":[\"file\",\"image_url\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\" \"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":[],\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":-0}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":0}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":0}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":0}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":0}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":0}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":9007199254740991}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":9007199254740991}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":9007199254740991}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":9007199254740992}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1e+21}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"live_probe\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"live_probe\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"live_probe\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"inferred\",\"verified_at_ms\":1000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"inferred\",\"verified_at_ms\":1000}}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"inferred\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\" catalog\",\"verified_at_ms\":1000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"source\":\"catalog\",\"verified_at_ms\":1,\"__proto__\":{}}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"source\":\"catalog\",\"verified_at_ms\":1,\"key\":null}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":4,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":4,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3.5,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":-0,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":-0}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":0}}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"\u00a0m\u00a0\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"m\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"\u00a0\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":5,\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":null,\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"verified_at_ms\":9,\"source\":\"live_probe\",\"reproducible_cycles\":3,\"conversation_id\":\"unknown\",\"token_refresh\":\"changed\",\"account_slots\":\"shared\",\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\"}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"shared\",\"token_refresh\":\"changed\",\"conversation_id\":\"unknown\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":9}}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"unknown\",\"token_refresh\":\"unknown\",\"conversation_id\":\"unknown\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"unknown\",\"token_refresh\":\"unknown\",\"conversation_id\":\"unknown\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5 \",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"s\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"catalog\",\"verified_at_ms\":2000}}]}",
   "normalized": "null",
   "compact": "null"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\"}]}",
   "normalized": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\"}]}",
   "compact": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}"
  },
  {
   "input": "{\"version\":1,\"providers\":[{\"id\":\"a\"},\"b\"]}",
   "normalized": "null",
   "compact": "null"
  }
 ],
 "mergeVectors": [
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"live_probe\",\"verified_at_ms\":1000}},{\"id\":\"d\"},{\"id\":\"a\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":5,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"live_probe\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"d\"},{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":5,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"live_probe\",\"verified_at_ms\":1000}},{\"id\":\"d\"},{\"id\":\"a\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":5,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":5,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"d\"}]}"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "false",
   "output": "false"
  },
  {
   "previous": "false",
   "next": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}"
  },
  {
   "previous": "false",
   "next": "false",
   "output": "false"
  },
  {
   "previous": "null",
   "next": "false",
   "output": "false"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "null",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "\"bogus\"",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}"
  },
  {
   "previous": "null",
   "next": "null",
   "output": "null"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\"}]}",
   "next": "{\"version\":1,\"providers\":[{\"id\":\"b\"}]}",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"b\"},{\"id\":\"a\"}]}"
  },
  {
   "previous": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
   "next": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}",
   "output": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}"
  }
 ],
 "mergeUndefined": {
  "prevUndefined": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
  "nextUndefined": "{\"version\":1,\"providers\":[{\"id\":\"a\",\"controls\":{\"key\":false,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"b\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"c\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"x\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}",
  "both": "null"
 },
 "payloadVectors": [
  {
   "input": "{\"updated_at_ms\":5,\"source\":\"s\",\"client_version\":\"1.2.3\",\"models\":[{\"prompt_cache\":{\"version\":1,\"providers\":[{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"z\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]},\"supported_reasoning_levels\":[\"low\",{\"effort\":\"ultra\"}],\"default_reasoning_level\":\"low\",\"effective_context_window_percent\":95,\"auto_compact_token_limit\":100,\"max_context_window\":200000,\"context_window\":100000,\"supported_in_api\":false,\"visibility\":\"list\",\"description\":\"d\",\"display_name\":\"D\",\"slug\":\"m1\",\"extra\":1}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"m1\",\"display_name\":\"D\",\"description\":\"d\",\"visibility\":\"list\",\"supported_in_api\":false,\"context_window\":100000,\"max_context_window\":200000,\"auto_compact_token_limit\":100,\"effective_context_window_percent\":95,\"default_reasoning_level\":\"low\",\"supported_reasoning_levels\":[\"low\",\"ultra\"],\"reasoning_effort_wire_map\":{\"ultra\":\"max\"},\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"z\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}}],\"source\":\"s\",\"updated_at_ms\":5,\"client_version\":\"1.2.3\"}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[{\"slug\":\"x\",\"display_name\":\"\",\"name\":\"N\"},{\"slug\":\"y\",\"displayName\":\"DN\",\"name\":\"N\"},{\"slug\":\"z\",\"name\":\"N\"},{\"slug\":\"w\",\"description\":\"\",\"visibility\":\"\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"x\"},{\"slug\":\"y\",\"display_name\":\"DN\"},{\"slug\":\"z\",\"display_name\":\"N\"},{\"slug\":\"w\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[{\"slug\":\"h1\",\"visibility\":\"H\u0130DE\"},{\"slug\":\"h2\",\"visibility\":\"\u00a0hide\u00a0\"},{\"slug\":\"h3\",\"visibility\":\"HIDE\u200b\"},{\"slug\":\"h4\",\"visibility\":\"Hide\",\"supported_in_api\":\"true\"},{\"slug\":\"h5\",\"visibility\":\"\uff28\uff29\uff24\uff25\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"h1\",\"visibility\":\"H\u0130DE\"},{\"slug\":\"h3\",\"visibility\":\"HIDE\u200b\"},{\"slug\":\"h5\",\"visibility\":\"\uff28\uff29\uff24\uff25\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[{\"slug\":\"r\",\"supported_reasoning_levels\":[{\"effort\":\"10\",\"wire_effort\":\"x\"},{\"effort\":\"2\",\"wire_effort\":\"y\"},{\"effort\":\"ultra\"},{\"effort\":\"a\",\"wire_effort\":\"b\"},{\"effort\":\"a\",\"wire_effort\":\"c\"},{\"effort\":\"low\",\"wire_effort\":\"low\"},{\"effort\":\"med\",\"wire_effort\":\"  high \"},{\"effort\":\" ultra \"},{\"effort\":\"x1\",\"wire_effort\":null},[\"low\"],{\"effort\":\"\"},\"\",\" \",{\"effort\":\"__proto__\",\"wire_effort\":\"p\"}]}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"r\",\"supported_reasoning_levels\":[\"10\",\"2\",\"ultra\",\"a\",\"a\",\"low\",\"med\",\"ultra\",\"x1\",\"__proto__\"],\"reasoning_effort_wire_map\":{\"2\":\"y\",\"10\":\"x\",\"ultra\":\"max\",\"a\":\"c\",\"med\":\"high\",\"__proto__\":\"p\"}}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[{\"slug\":\"c\",\"context_window\":-0.5,\"max_context_window\":-0,\"auto_compact_token_limit\":1e+21,\"effective_context_window_percent\":-1},{\"slug\":\"d\",\"context_window\":1500,\"max_context_window\":\"5\",\"auto_compact_token_limit\":true,\"effective_context_window_percent\":0.999}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"c\",\"context_window\":0,\"max_context_window\":0,\"auto_compact_token_limit\":1e+21},{\"slug\":\"d\",\"context_window\":1500,\"effective_context_window_percent\":0}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":-0.5,\"models\":[{\"slug\":\"t\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":0}"
  },
  {
   "input": "{\"updated_at_ms\":1e+300,\"models\":[{\"slug\":\"t\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":1e+300}"
  },
  {
   "input": "{\"updated_at_ms\":5.9,\"models\":[{\"slug\":\"t\"}]}",
   "overrides": "{\"updatedAtMs\":\"__INFINITY__\"}",
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5.9,\"models\":[{\"slug\":\"t\"}]}",
   "overrides": "{\"updatedAtMs\":-7.5}",
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":-7}"
  },
  {
   "input": "{\"updated_at_ms\":5.9,\"models\":[{\"slug\":\"t\"}]}",
   "overrides": "{\"updatedAtMs\":0}",
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":0}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"source\":\"\",\"models\":[{\"slug\":\"t\"}]}",
   "overrides": "{\"source\":\"\"}",
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"source\":5,\"client_version\":7,\"clientVersion\":\"8.8.8\",\"models\":[{\"slug\":\"t\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5,\"client_version\":\"8.8.8\"}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"client_version\":\"\",\"clientVersion\":\"8.8.8\",\"models\":[{\"slug\":\"t\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5,\"client_version\":\"\"}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"client_version\":\"\",\"models\":[{\"slug\":\"t\"}]}",
   "overrides": "{\"clientVersion\":null}",
   "output": "{\"models\":[{\"slug\":\"t\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5,\"client_version\":\"\"}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":\"x\",\"data\":[{\"slug\":\"from-data\"}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"from-data\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[],\"data\":[{\"slug\":\"from-data\"}]}",
   "overrides": null,
   "output": "null"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[{\"slug\":5,\"id\":\"x\"},{\"slug\":\"a\"},{\"slug\":\" a\"},{\"id\":\"a\"},{\"slug\":\"\",\"id\":\"e\"},[],[\"x\"]]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"x\"},{\"slug\":\"a\"},{\"slug\":\" a\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "{\"updated_at_ms\":5,\"models\":[{\"slug\":\"p1\",\"prompt_cache\":false},{\"slug\":\"p2\",\"prompt_cache\":true},{\"slug\":\"p3\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"a\"},{\"id\":\"a\"}]}},{\"slug\":\"p4\",\"prompt_cache\":null}]}",
   "overrides": null,
   "output": "{\"models\":[{\"slug\":\"p1\",\"prompt_cache\":false},{\"slug\":\"p2\"},{\"slug\":\"p3\"},{\"slug\":\"p4\"}],\"source\":\"codex_cli\",\"updated_at_ms\":5}"
  },
  {
   "input": "[{\"slug\":\"arr\",\"updated_at_ms\":9}]",
   "overrides": "{\"updatedAtMs\":3,\"source\":\"o\",\"clientVersion\":\"1.0.0\"}",
   "output": "{\"models\":[{\"slug\":\"arr\"}],\"source\":\"o\",\"updated_at_ms\":3,\"client_version\":\"1.0.0\"}"
  }
 ],
 "payloadNegZero": [
  [],
  [],
  [],
  [],
  [
   "0.context_window",
   "0.max_context_window"
  ],
  [
   "updated_at_ms"
  ],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  [],
  []
 ],
 "modelMergeVectors": [
  {
   "next": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\"},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"n\"}]},\"slug\":\"b\",\"x\":1},{\"slug\":\"c\",\"prompt_cache\":\"bogus\"},\"str\",{\"id\":\"  a  \"}],\"updated_at_ms\":1,\"client_version\":\"1.0.0\"}",
   "previous": "{\"source\":\"s\",\"models\":[{\"slug\":\"  a  \",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}},{\"slug\":\"a\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"dup\"}]}},{\"slug\":\"b\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"q\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]}},{\"slug\":\"c\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"r\"}]}}],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"n\"},{\"id\":\"q\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}}]},\"slug\":\"b\",\"x\":1},{\"slug\":\"c\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"r\"}]}},\"str\",{\"id\":\"  a  \",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}}],\"updated_at_ms\":1,\"client_version\":\"1.0.0\"}",
   "same": false
  },
  {
   "next": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":false}],\"updated_at_ms\":1}",
   "previous": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\"}]}}],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":false}],\"updated_at_ms\":1}",
   "same": false
  },
  {
   "next": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\"}],\"updated_at_ms\":1}",
   "previous": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":false}],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":false}],\"updated_at_ms\":1}",
   "same": false
  },
  {
   "next": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\"}],\"updated_at_ms\":1}",
   "previous": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":\"bogus\"}],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\"}],\"updated_at_ms\":1}",
   "same": true
  },
  {
   "next": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\"}],\"updated_at_ms\":1}",
   "previous": "{\"source\":\"s\",\"models\":[[{\"slug\":\"a\"}]],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\"}],\"updated_at_ms\":1}",
   "same": true
  },
  {
   "next": "{\"source\":\"s\",\"models\":[{\"slug\":\"\",\"id\":\"a\"}],\"updated_at_ms\":1}",
   "previous": "{\"source\":\"s\",\"models\":[{\"slug\":\"a\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\"}]}}],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"slug\":\"\",\"id\":\"a\"}],\"updated_at_ms\":1}",
   "same": true
  },
  {
   "next": "{\"source\":\"s\",\"models\":[{\"name\":\"a\"}],\"updated_at_ms\":1}",
   "previous": "{\"source\":\"s\",\"models\":[{\"model\":\"a\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\"}]}}],\"updated_at_ms\":1}",
   "output": "{\"source\":\"s\",\"models\":[{\"name\":\"a\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"p\"}]}}],\"updated_at_ms\":1}",
   "same": false
  }
 ],
 "typeErrors": [
  {
   "models": null,
   "undefinedModels": true,
   "error": "TypeError: undefined is not an object (evaluating 'next.models.map')"
  },
  {
   "models": "null",
   "undefinedModels": false,
   "error": "TypeError: null is not an object (evaluating 'next.models.map')"
  },
  {
   "models": "\"none\"",
   "undefinedModels": false,
   "error": "TypeError: next.models.map is not a function. (In 'next.models.map((value) => mergeCodexModelPromptCache(value, previousBySlug))', 'next.models.map' is undefined)"
  },
  {
   "models": "5",
   "undefinedModels": false,
   "error": "TypeError: next.models.map is not a function. (In 'next.models.map((value) => mergeCodexModelPromptCache(value, previousBySlug))', 'next.models.map' is undefined)"
  },
  {
   "models": "{\"a\":1}",
   "undefinedModels": false,
   "error": "TypeError: next.models.map is not a function. (In 'next.models.map((value) => mergeCodexModelPromptCache(value, previousBySlug))', 'next.models.map' is undefined)"
  }
 ],
 "noThrowWhenPreviousEmpty": "{\"source\":\"s\",\"models\":\"none\",\"updated_at_ms\":1}",
 "withVectors": [
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": " target ",
   "provider": "codex_chatgpt",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"  trimmed  \",\"reproducible_cycles\":5,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"codex_chatgpt\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"trimmed\",\"reproducible_cycles\":5,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}"
  },
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": "target",
   "provider": "first",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000},\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"codex_chatgpt\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}"
  },
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": "target",
   "provider": "last",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":0}",
   "output": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"id\":\"codex_chatgpt\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}},{\"id\":\"last\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":0}}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}"
  },
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": "o",
   "provider": "codex_chatgpt",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}"
  },
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": "target",
   "provider": "codex_chatgpt",
   "scope": "{\"probe_profile\":\"x\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "null"
  },
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": "target",
   "provider": "missing",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "null"
  },
  {
   "snapshot": "{\"updated_at_ms\":1,\"models\":[{\"slug\":\"o\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}},{\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"first\",\"controls\":{\"key\":true,\"implicit\":true,\"explicit_breakpoints\":true,\"modes\":[\"implicit\"],\"ttls\":[\"30m\"],\"legacy_retentions\":[\"24h\"],\"breakpoint_block_types\":{\"responses\":[\"input_text\"],\"chat_completions\":[\"text\"]},\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1000}},{\"scope\":{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"old\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000},\"id\":\"codex_chatgpt\"},{\"id\":\"last\"}]},\"id\":\"  target  \",\"tail\":true},\"x\"],\"source\":\"src\",\"extra\":{\"k\":1}}",
   "slug": "x",
   "provider": "codex_chatgpt",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "null"
  },
  {
   "snapshot": "{\"source\":\"s\",\"models\":[{\"slug\":\"d\"},{\"id\":\"d\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\"}]}}],\"updated_at_ms\":1}",
   "slug": "d",
   "provider": "codex_chatgpt",
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "output": "null"
  }
 ],
 "lookupVectors": [
  {
   "slug": "e",
   "unique": "null",
   "provider": "null",
   "eligible": false
  },
  {
   "slug": "y",
   "unique": "{\"slug\":5,\"id\":\" y \"}",
   "provider": "null",
   "eligible": false
  },
  {
   "slug": " y ",
   "unique": "{\"slug\":5,\"id\":\" y \"}",
   "provider": "null",
   "eligible": false
  },
  {
   "slug": "dup",
   "unique": "null",
   "provider": "null",
   "eligible": false
  },
  {
   "slug": "pc",
   "unique": "{\"slug\":\"pc\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"source\":\"catalog\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cache_write_tokens\",\"cached_tokens\"]}}]}}",
   "provider": "{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"expected_usage_fields\":[\"cache_write_tokens\",\"cached_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1}}",
   "eligible": true
  },
  {
   "slug": "pc2",
   "unique": "{\"slug\":\"pc2\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"implicit\":true,\"source\":\"catalog\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cached_tokens\"]}}]}}",
   "provider": "{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"implicit\":true,\"expected_usage_fields\":[\"cached_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1}}",
   "eligible": false
  },
  {
   "slug": "pc3",
   "unique": "{\"slug\":\"pc3\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":\"true\",\"source\":\"catalog\",\"verified_at_ms\":1}}]}}",
   "provider": "null",
   "eligible": false
  },
  {
   "slug": "pc4",
   "unique": "{\"slug\":\"pc4\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"implicit\":false,\"source\":\"catalog\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"]}}]}}",
   "provider": "{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"implicit\":false,\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"catalog\",\"verified_at_ms\":1}}",
   "eligible": false
  },
  {
   "slug": "pc5",
   "unique": "{\"slug\":\"pc5\",\"prompt_cache\":{\"version\":1,\"providers\":[{\"id\":\"other\"},{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"source\":\"inferred\",\"verified_at_ms\":1,\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"modes\":[]}}]}}",
   "provider": "{\"id\":\"codex_chatgpt\",\"controls\":{\"key\":true,\"modes\":[],\"expected_usage_fields\":[\"cached_tokens\",\"cache_write_tokens\"],\"source\":\"inferred\",\"verified_at_ms\":1}}",
   "eligible": true
  },
  {
   "slug": "",
   "unique": "null",
   "provider": "null",
   "eligible": false
  },
  {
   "slug": "5",
   "unique": "null",
   "provider": "null",
   "eligible": false
  }
 ],
 "defaultModels": [
  {
   "snapshot": "{\"source\":\"s\",\"models\":[{\"slug\":\"\",\"id\":\"x\"},{\"slug\":5,\"id\":\" y \"}],\"updated_at_ms\":1}",
   "output": "\"y\""
  },
  {
   "snapshot": "{\"source\":\"s\",\"models\":[[],{\"name\":\"\u00a0n\u00a0\"}],\"updated_at_ms\":1}",
   "output": "\"n\""
  },
  {
   "snapshot": "{\"source\":\"s\",\"models\":[{\"slug\":\"\u2028\"},{\"id\":\"\\tz\"}],\"updated_at_ms\":1}",
   "output": "\"z\""
  },
  {
   "snapshot": "{\"source\":\"s\",\"models\":{\"0\":{\"slug\":\"a\"}},\"updated_at_ms\":1}",
   "output": "null"
  }
 ],
 "concrete": [
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": null,
   "output": true
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": 3,
   "output": true
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":4,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": 4,
   "output": true
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":4,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": 3,
   "output": false
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"\u00a0\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": null,
   "output": false
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"shared\",\"token_refresh\":\"changed\",\"conversation_id\":\"independent\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": null,
   "output": true
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"effective_model\":\"gpt-5-codex\",\"reproducible_cycles\":3,\"source\":\"Live_probe\",\"verified_at_ms\":2000}",
   "cycles": null,
   "output": false
  },
  {
   "scope": "{\"probe_profile\":\"responses_implicit_input_text_keyed_cycle_isolated_v5\",\"account_slots\":\"account_scoped\",\"token_refresh\":\"preserved\",\"conversation_id\":\"scoped\",\"reproducible_cycles\":3,\"source\":\"live_probe\",\"verified_at_ms\":2000}",
   "cycles": null,
   "output": false
  }
 ],
 "concreteNaN": false,
 "versionVectors": [
  {
   "value": "1.2.3",
   "parsed": "[1,2,3]"
  },
  {
   "value": "01.002.0003",
   "parsed": "[1,2,3]"
  },
  {
   "value": " \t1.2.3\n",
   "parsed": "[1,2,3]"
  },
  {
   "value": "\u00a01.2.3\u00a0",
   "parsed": "[1,2,3]"
  },
  {
   "value": "1.2.3\u2028",
   "parsed": "[1,2,3]"
  },
  {
   "value": "\uff11.2.3",
   "parsed": "null"
  },
  {
   "value": "1.2.3.4",
   "parsed": "null"
  },
  {
   "value": "9007199254740991.0.0",
   "parsed": "[9007199254740991,0,0]"
  },
  {
   "value": "9007199254740992.0.0",
   "parsed": "null"
  },
  {
   "value": "",
   "parsed": "null"
  },
  {
   "value": "1..3",
   "parsed": "null"
  },
  {
   "value": "1.2.-3",
   "parsed": "null"
  },
  {
   "value": "+1.2.3",
   "parsed": "null"
  },
  {
   "value": "1.2.3 ",
   "parsed": "[1,2,3]"
  },
  {
   "value": "10.0.0",
   "parsed": "[10,0,0]"
  },
  {
   "value": "2.0.0",
   "parsed": "[2,0,0]"
  },
  {
   "value": "1.2.30",
   "parsed": "[1,2,30]"
  },
  {
   "value": "0.0.0",
   "parsed": "[0,0,0]"
  },
  {
   "value": "1e3.0.0",
   "parsed": "null"
  }
 ],
 "comparePairs": [
  {
   "left": "10.0.0",
   "right": "2.0.0",
   "output": 8
  },
  {
   "left": "1.2.3",
   "right": "1.2.30",
   "output": -27
  },
  {
   "left": "0.0.0",
   "right": "0.0.0",
   "output": 0
  },
  {
   "left": "9007199254740991.0.0",
   "right": "0.0.0",
   "output": 9007199254740991
  },
  {
   "left": "1.2.3",
   "right": "1.2.3.4",
   "output": null
  },
  {
   "left": "01.2.3",
   "right": "1.2.3",
   "output": 0
  }
 ]
})VECTORS";
