from pathlib import Path
import re
r=Path.cwd();crates=r/'rust/crates';donor=crates/'gateway-extras-runtime';support=crates/'gateway-extras-runtime-test-support'
s=(donor/'tests/runtime.rs').read_text();a=s.index('fn poll<');b=s.index('fn principal(');block=s[a:b]
imports='''//! Original runtime fixture host with real dev-only AES-GCM; no production consumer.
use aes_gcm::{Aes256Gcm, KeyInit, Nonce, aead::Aead};
use crate::fixture::error;
use gateway_source::error::GatewayError;
use gateway_core::lifecycle::SemanticTerminal;
use gateway_extras_contract::types::{DispatchFailure, ExtrasDispatch, ExtrasHost, ExtrasProvider, Headers, ImageKind, UpstreamResponse};
use gateway_providers::types::PrepareOptions;
use serde_json::{Value, json};
use std::{cell::{Cell, RefCell}, collections::VecDeque, future::Future, pin::Pin, rc::Rc, task::{Context, Poll, Waker}};
'''
block=block.replace('fn poll<','pub fn poll<').replace('fn run<','pub fn run<').replace('struct Host(Rc<HostState>);','pub struct Host(pub Rc<HostState>);').replace('struct HostState {','pub struct HostState {')
fields_a=block.index('pub struct HostState {');fields_b=block.index('\n}',fields_a)
block=block[:fields_a]+re.sub(r'(?m)^    (\w+:)',r'    pub \1',block[fields_a:fields_b])+block[fields_b:]
for name in ['new','reply','count']:block=block.replace('    fn '+name+'(', '    pub fn '+name+'(')
(support/'src/host.rs').write_text(imports+block)
(support/'src/lib.rs').write_text('//! Dev-only Extras store and actual cryptographic runtime fixtures.\npub mod fixture;\npub mod host;\n')
s=s[:a]+s[b:]
# Retain every runtime case, queued_job, assertion and literal; only defining imports move.
a=s.index('fn principal(')
imports='''use gateway_auth_primitives::api_keys::ApiKeyPolicy;
use gateway_auth_primitives::crypto::Secret;
use gateway_auth_contracts::types::{AuthMethod, AuthPrincipal};
use gateway_source::error::GatewayError;
use gateway_core::identity::{ApiKeyId, PrincipalId, RequestContext, RequestId};
use gateway_core::lifecycle::SemanticTerminal;
use gateway_core::state::RecordKey;
use gateway_core::tuple_key::TupleKey;
use gateway_extras_runtime_test_support::fixture::{Memory, error};
use gateway_extras_runtime_test_support::host::{Host, poll, run};
use gateway_extras_contract::types::{DispatchFailure, DispatchFailureOrigin, ExtrasRequest, ExtrasRoute, Headers, HealthKind, ImageKind, KernelDisposition};
use gateway_extras_runtime::ExtrasRuntime;
use serde_json::{Value, json};
use std::{cell::Cell, rc::Rc};
'''
s=imports+s[a:]
s=s.replace('gateway_auth::crypto::','gateway_auth_primitives::crypto::').replace('gateway_core::source_json::','gateway_source::source_json::').replace('PrincipalId(format!("api-key:{id}"))','PrincipalId::from_scalar(format!("api-key:{id}"))')
(donor/'tests/runtime.rs').write_text(s)
s=(support/'src/fixture.rs').read_text().replace('gateway_core::error::','gateway_source::error::');(support/'src/fixture.rs').write_text(s)
s=(donor/'tests/decisions.rs').read_text();a=s.index('use gateway_extras_contract')
s='''use gateway_auth_primitives::api_keys::ApiKeyPolicy;
use gateway_auth_contracts::types::{AuthMethod, AuthPrincipal};
use gateway_core::{identity::{PrincipalId, RequestContext, RequestId}, model::ModelId};
use gateway_source::{error::GatewayError, source_json::SourceJson};
'''+s[a:]
s=s.replace('PrincipalId("fixture".into())','PrincipalId::from_scalar("fixture".into())')
(donor/'tests/decisions.rs').write_text(s)
