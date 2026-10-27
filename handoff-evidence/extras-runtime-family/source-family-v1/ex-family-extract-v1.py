from pathlib import Path
import re
r=Path.cwd(); crates=r/'rust/crates'; donor=crates/'gateway-extras-runtime'
# Bound source mover: no application, parser, test driver, compiler or external tools.
def span(s,start):
 stack=[]; quote=False; esc=False
 for i in range(start,len(s)):
  c=s[i]
  if quote:
   if esc: esc=False
   elif c=='\\': esc=True
   elif c=='"': quote=False
   continue
  if c=='"': quote=True; continue
  if c in '({[': stack.append(c)
  elif c in ')}]':
   assert stack and '({['[')}]'.index(c)]==stack.pop(),s[max(0,i-100):i+100]
   if not stack:return i+1
 raise ValueError('unclosed')
def remove_fn(s,name):
 m=re.search(r'pub\(crate\) (?:async )?fn '+name+r'\b',s);assert m,name
 a=m.start(); b=span(s,s.index('{',a));return s[:a]+s[b+1:]
def port(s):
 s=s.replace('gateway_auth::crypto','gateway_auth_primitives::crypto').replace('gateway_auth::api_keys','gateway_auth_primitives::api_keys').replace('gateway_auth::types','gateway_auth_contracts::types')
 s=s.replace('gateway_core::error','gateway_source::error').replace('gateway_core::js_string','gateway_source::js_string').replace('gateway_core::source_json','gateway_source::source_json')
 return s
for c in ['gateway-extras-primitives','gateway-extras-state','gateway-extras-jobs','gateway-extras-voyage']:(crates/c/'src').mkdir(parents=True,exist_ok=True)
# Shared finite data/host primitives retain the donor definitions. Mutation helpers are
# not exported: state/jobs call the existing defining Auth store operations instead.
s=(donor/'src/store.rs').read_text()
for name in ['check','put','commit']:s=remove_fn(s,name)
s=port(s).replace('pub(crate) ','pub ')
a=s.index('use gateway_core::{');b=s.index('use gateway_extras_contract::errors',a)
s=s[:a]+'use gateway_core::{state::{RecordKey, StoredRecord}, tuple_key::TupleKey};\nuse gateway_source::error::GatewayError;\n'+s[b:]
s=s.replace('use serde::{Serialize, de::DeserializeOwned};','use serde::de::DeserializeOwned;')
(crates/'gateway-extras-primitives/src/lib.rs').write_text('//! Extras-only value decoding, source hash inputs, clock and random primitives.\n//! No store commit, raw-key mutation, credential or dispatch capability is exposed.\npub mod value;\n')
(crates/'gateway-extras-primitives/src/value.rs').write_text(s)
# Replace the private donor put helper with the actual existing Auth defining API.
# Preserve uuid-before-serialization order and the original Extras serialization error.
def puts(s):
 count=0; at=0
 while True:
  m=re.search(r'\bput\(',s[at:])
  if not m:break
  a=at+m.start(); begin=a+3;b=span(s,begin);raw=s[begin+1:b-1]
  args=[];start=0;i=0
  while i<len(raw):
   if raw[i]=='"':
    i+=1
    while i<len(raw):
     if raw[i]=='\\':i+=2;continue
     if raw[i]=='"':i+=1;break
     i+=1
    continue
   if raw[i] in '({[':i=span(raw,i);continue
   if raw[i]==',':args.append(raw[start:i].strip());start=i+1
   i+=1
  if raw[start:].strip():args.append(raw[start:].strip())
  assert len(args)==4 and args[0]=='host',args
  assert s[b]=='?'
  replacement='put('+', '.join(args[1:]+['&uuid(host)?'])+').map_err(|_| invalid_state())'
  s=s[:a]+replacement+s[b:];at=a+len(replacement)+1;count+=1
 return s,count
counts={}
for f in ['cache','idempotency']:
 s=(donor/f'src/{f}.rs').read_text()
 s=re.sub(r'use crate::store::\{[^}]+\};\n','',s,count=1)
 helpers={'cache':'decoded, key, now, uuid','idempotency':'decoded, hash, key, now, uuid'}[f]
 s='use gateway_auth_primitives::store::{check, commit, put};\nuse gateway_extras_primitives::value::{'+helpers+'};\n'+s
 s=port(s);s,n=puts(s);counts[f]=n
 s=s.replace('pub(crate) ','pub ')
 if f=='cache':
  s=s.replace('pub fn cache_key','fn cache_key').replace('pub fn vector','fn vector').replace('pub async fn reserve_once','async fn reserve_once').replace('crate::store::source_utf8','gateway_extras_primitives::value::source_utf8')
 (crates/f'gateway-extras-state/src/{f}.rs').write_text(s)
(crates/'gateway-extras-state/src/lib.rs').write_text('//! Complete Extras cache/budget and acquire-only idempotency state operations.\n//! Keys, ledgers and Lease fields remain private to their defining modules.\npub mod cache;\npub mod idempotency;\n')
# Cut jobs -> runtime: the complete fixed Voyage helpers get one lower owner.
s=(donor/'src/embeddings.rs').read_text();a=s.index('pub(crate) fn configured_error');b=s.index('struct SyncLease')
ops=s[a:b].replace('pub(crate) ','pub ')
imports='''//! Fixed Voyage dispatch/response operations shared by sync and poll-driven jobs.
//! Uses only the original factory-only PreparedCall; retry/header parsing is private.
use chrono::{DateTime, Utc};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_extras_contract::errors::server;
use gateway_extras_contract::types::{DispatchFailure, DispatchFailureOrigin, ExtrasDispatch, ExtrasHost, ExtrasProvider, Headers, PreparedCall};
use gateway_providers::extras::{self, EmbeddingsProfile};
use gateway_providers::types::PrepareOptions;
use serde_json::{Value, json};
'''
(crates/'gateway-extras-voyage/src/lib.rs').write_text(imports+ops)
# Retained sync orchestration, preserving its private SyncLease methods and loop.
imports='''use gateway_extras_primitives::value::{hash, hash_source, js_whitespace, now, retry_seconds, source_utf8, uuid};
use gateway_extras_state::{cache::{TIMEOUT_MS, cache_best_effort, cached, chunks, estimate, reserve}, idempotency::{self, Acquired, Lease}};
use gateway_extras_voyage::{configured_error, fetch_chunk, result_body, retryable, usage};
use gateway_core::{lifecycle::SemanticTerminal, query::QueryStore};
use gateway_source::{error::GatewayError, js_string::JsString16};
use gateway_extras_contract::errors::{cancelled, invalid, server};
use gateway_extras_contract::types::{ExtrasHost, ExtrasProvider, ExtrasRequest, ExtrasResponse};
use gateway_providers::extras;
use serde_json::json;
use std::collections::BTreeMap;
'''
s=imports+s[b:]
s=s.replace('let principal = request.context.principal.0.trim_matches(js_whitespace);','let principal = source_utf8(request.context.principal.as_source());\n        let principal = principal.trim_matches(js_whitespace);')
(donor/'src/embeddings.rs').write_text(s)
# Jobs retains its full keys, records, seed/key/crypto functions, attempt and runner lease.
s=(donor/'src/jobs.rs').read_text();a=s.index('use base64')
imports='''//! Complete retained embedding jobs; only request create/get can run an attempt.
use gateway_auth_primitives::store::{check, commit, put};
use gateway_extras_primitives::value::{decoded, hash, hash_source, key, now, retry_seconds, source_utf8, uuid};
use gateway_extras_state::cache::{TIMEOUT_MS, cache_best_effort, cached, chunks, estimate, reserve};
use gateway_extras_voyage::{configured_error, fetch_chunk, result_body, retryable, usage};
'''
s=port(imports+s[a:]);s=s.replace('use gateway_auth::{crypto::Secret, types::AuthMethod};','use gateway_auth_primitives::crypto::Secret;\nuse gateway_auth_contracts::types::AuthMethod;')
s=s.replace('    error::GatewayError,\n','').replace('    js_string::JsString16,\n','')
s=s.replace('use gateway_extras_contract::errors::server;','use gateway_source::{error::GatewayError, js_string::JsString16};\nuse gateway_extras_contract::errors::{invalid_state, server};')
s=s.replace('pub(crate) struct Job','struct Job').replace('pub(crate) fn job_key','fn job_key').replace('pub(crate) fn lookup_key','fn lookup_key').replace('pub(crate) async fn ','pub async fn ')
s,n=puts(s);counts['jobs']=n
(crates/'gateway-extras-jobs/src/lib.rs').write_text(s)
# Runtime composes the direct defining jobs owner. No Worker lifetime path is touched.
s=(donor/'src/lib.rs').read_text().replace('mod cache;\n','').replace('mod idempotency;\n','').replace('mod jobs;\n','').replace('mod store;\n','')
s=s.replace('use gateway_core::{error::GatewayError, query::QueryStore};','use gateway_core::query::QueryStore;\nuse gateway_source::error::GatewayError;\nuse gateway_extras_jobs::{create, get};\nuse gateway_extras_primitives::value::js_whitespace;')
s=s.replace('jobs::create(', 'create(').replace('jobs::get(', 'get(').replace('store::js_whitespace','js_whitespace')
(donor/'src/lib.rs').write_text(s)
s=(donor/'src/images.rs').read_text().replace('use gateway_core::{error::GatewayError, source_json::SourceJson, source_serialization as source};','use gateway_source::{error::GatewayError, source_json::SourceJson, source_serialization as source};')
(donor/'src/images.rs').write_text(s)
for f in ['cache','idempotency','jobs','store']:(donor/f'src/{f}.rs').unlink()
print('Complete owner moves, original put sites consumed via existing Auth primitive API:',counts)
