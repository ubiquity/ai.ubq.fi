from pathlib import Path
r=Path.cwd();crates=r/'rust/crates'
header='''[package]
name = "{name}"
version.workspace = true
edition.workspace = true
rust-version.workspace = true
publish.workspace = true

[dependencies]
'''
base={
'gateway-extras-primitives':['gateway-auth-primitives','gateway-core','gateway-extras-contract','gateway-source','serde','serde_json'],
'gateway-extras-state':['gateway-auth-primitives','gateway-core','gateway-extras-contract','gateway-extras-primitives','gateway-source','serde','serde_json'],
'gateway-extras-jobs':['gateway-auth-contracts','gateway-auth-primitives','gateway-core','gateway-extras-contract','gateway-extras-primitives','gateway-extras-state','gateway-extras-voyage','gateway-providers','gateway-source','serde','serde_json'],
'gateway-extras-voyage':['gateway-extras-contract','gateway-providers','gateway-source','serde','serde_json'],
'gateway-extras-runtime':['gateway-core','gateway-decision-runtime','gateway-extras-contract','gateway-extras-jobs','gateway-extras-primitives','gateway-extras-state','gateway-extras-voyage','gateway-providers','gateway-schema','gateway-source','serde','serde_json'],
'gateway-extras-runtime-test-support':['gateway-core','gateway-extras-contract','gateway-providers','gateway-source','serde_json'],
}
extra={
'gateway-extras-state':'chrono = { version = "=0.4.45", default-features = false, features = ["std"] }\n',
'gateway-extras-voyage':'chrono = { version = "=0.4.45", default-features = false, features = ["std"] }\n',
'gateway-extras-jobs':'sha2 = "=0.10.9"\nbase64 = "=0.22.1"\n',
'gateway-extras-runtime':'futures-util = { version = "=0.3.34", default-features = false, features = ["std"] }\n',
'gateway-extras-runtime-test-support':'aes-gcm = { version = "=0.10.3", default-features = false, features = ["aes", "alloc"] }\n',
}
for c, deps in base.items():
 body=header.format(name=c)+''.join(n+'.workspace = true\n' for n in deps)+extra.get(c,'')
 if c=='gateway-extras-runtime':body+='\n[dev-dependencies]\ngateway-auth-contracts.workspace = true\ngateway-auth-primitives.workspace = true\ngateway-extras-runtime-test-support.workspace = true\n'
 (crates/c/'Cargo.toml').write_text(body+'\n[lints]\nworkspace = true\n')
new=['gateway-extras-primitives','gateway-extras-state','gateway-extras-jobs','gateway-extras-voyage']
p=r/'rust/Cargo.toml';s=p.read_text()
marker='  "crates/gateway-extras-runtime",'
assert s.count(marker)==1
s=s.replace(marker,marker+'\n'+''.join('  "crates/'+c+'",\n' for c in new).rstrip())
marker='gateway-extras-runtime = { path = "crates/gateway-extras-runtime" }'
assert s.count(marker)==1
s=s.replace(marker,marker+'\n'+'\n'.join(c+' = { path = "crates/'+c+'" }' for c in new));p.write_text(s)
# Exact first-party lock blocks only. No lock generation, external block edits or version change.
p=r/'rust/Cargo.lock';s=p.read_text();blocks=s.split('[[package]]\n');before=blocks[0];byname={}
for b in blocks[1:]:
 name=b.split('name = "',1)[1].split('"',1)[0];assert name not in byname or not name.startswith('gateway-')
 if name in base:byname[name]=b
replacements={
 'gateway-extras-primitives':['gateway-auth-primitives','gateway-core','gateway-extras-contract','gateway-source','serde','serde_json'],
 'gateway-extras-state':['chrono','gateway-auth-primitives','gateway-core','gateway-extras-contract','gateway-extras-primitives','gateway-source','serde','serde_json'],
 'gateway-extras-jobs':['base64 0.22.1','gateway-auth-contracts','gateway-auth-primitives','gateway-core','gateway-extras-contract','gateway-extras-primitives','gateway-extras-state','gateway-extras-voyage','gateway-providers','gateway-source','serde','serde_json','sha2 0.10.9'],
 'gateway-extras-voyage':['chrono','gateway-extras-contract','gateway-providers','gateway-source','serde','serde_json'],
 'gateway-extras-runtime':['futures-util','gateway-auth-contracts','gateway-auth-primitives','gateway-core','gateway-decision-runtime','gateway-extras-contract','gateway-extras-jobs','gateway-extras-primitives','gateway-extras-runtime-test-support','gateway-extras-state','gateway-extras-voyage','gateway-providers','gateway-schema','gateway-source','serde','serde_json'],
 'gateway-extras-runtime-test-support':['aes-gcm','gateway-core','gateway-extras-contract','gateway-providers','gateway-source','serde_json'],
}
for c,deps in replacements.items():
 block='name = "'+c+'"\nversion = "0.1.0"\ndependencies = [\n'+''.join(' "'+d+'",\n' for d in sorted(deps))+']\n\n'
 if c in byname:s=s.replace('[[package]]\n'+byname[c],'[[package]]\n'+block)
 else:
  # Insert before the first lexicographically larger package name.
  candidates=[b for b in s.split('[[package]]\n')[1:] if b.startswith('name = "') and b.split('"')[1]>c]
  assert candidates
  s=s.replace('[[package]]\n'+candidates[0],'[[package]]\n'+block+'[[package]]\n'+candidates[0],1)
p.write_text(s)
# Current bc8 defining paths, no adapters or fallback constructors.
p=crates/'gateway-extras-state/src/idempotency.rs';s=p.read_text().replace('    error::GatewayError,\n','');s='use gateway_source::error::GatewayError;\n'+s;p.write_text(s)
p=crates/'gateway-extras-runtime/tests/decisions.rs';s=p.read_text().replace('PrincipalId("api-key:fixture".into())','PrincipalId::from_scalar("api-key:fixture".into())');p.write_text(s)
print('Registered exactly four new family owners and rewrote only six own first-party lock package blocks.')
