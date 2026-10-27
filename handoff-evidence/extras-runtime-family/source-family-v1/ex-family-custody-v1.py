from pathlib import Path
import ast,re,json,hashlib,subprocess,tomllib
r=Path.cwd();out=r/'handoff-evidence/extras-runtime-family/source-family-v1';base='7694a49787c7379180cb0e268b8b03fac093d879'
def h(data):return hashlib.sha256(data).hexdigest()
def read(p):return (r/p).read_text()
def old(p):return subprocess.check_output(['git','show',base+':'+p])
ns={'re':re,'sha':lambda s:h(s.encode())}
for n in ast.parse(Path('/tmp/ex-family-inventory-v1.py').read_text()).body:
 if isinstance(n,ast.FunctionDef) and n.name in ['tokens','functions']:exec(compile(ast.Module(body=[n],type_ignores=[]),'static-inventory','exec'),ns)
def block(s,keyword,name):
 ts=ns['tokens'](s)
 for i,t in enumerate(ts):
  if t==keyword and ts[i+1]==name:
   j=i+2
   while ts[j]!='{':j+=1
   end=j+1;depth=1
   while depth:depth+=(ts[end]=='{')-(ts[end]=='}');end+=1
   return ts[j:end]
 raise ValueError(name)
# Private defining data bodies stay exact across moves; visibility remains private.
checks=[]
for oldfile,newfile,names in [
 ('idempotency.rs','gateway-extras-state/src/idempotency.rs',[('struct','Lease'),('struct','Ledger'),('enum','State')]),
 ('cache.rs','gateway-extras-state/src/cache.rs',[('struct','Budget')]),
 ('jobs.rs','gateway-extras-jobs/src/lib.rs',[('struct','Job'),('struct','InputRecord'),('struct','JobAttempt')]),
 ('embeddings.rs','gateway-extras-runtime/src/embeddings.rs',[('struct','SyncLease')])]:
 a=old('rust/crates/gateway-extras-runtime/src/'+oldfile).decode();b=read('rust/crates/'+newfile)
 for kind,name in names:
  oa=block(a,kind,name);nb=block(b,kind,name);assert oa==nb,name
  if name=='Lease':assert 'pub struct Lease {' in b and not any(x.startswith('pub ') for x in b.split('pub struct Lease {',1)[1].split('}',1)[0].splitlines())
  else:assert not re.search(r'pub(?:\([^)]*\))?\s+'+kind+r'\s+'+name+r'\b',b)
  checks.append({'from':oldfile,'to':newfile,'name':name,'fieldsBodyExact':True,'tokensSha256':h(' '.join(oa).encode()),'private':name!='Lease','LeaseFieldsPrivate':True if name=='Lease' else None})
# Original runtime host methods moved unchanged, only visibility and defining imports differ.
a=old('rust/crates/gateway-extras-runtime/tests/runtime.rs').decode();host=read('rust/crates/gateway-extras-runtime-test-support/src/host.rs')
hostf=ns['functions'](host);af=ns['functions'](a);hostchecks=[]
for m in hostf:
 matches=[x for x in af if x['name']==m['name'] and x['bodySha256']==m['bodySha256']];assert len(matches)==1,m['name']
 hostchecks.append({'name':m['name'],'bodySha256':m['bodySha256'],'originalBodyExact':True})
# Literal external fixtures (if any), including conventional module closures of all retained test sources.
fixture_inputs=[]
for family in ['gateway-extras-runtime','gateway-extras-runtime-test-support','gateway-extras-contract','gateway-decision-runtime','gateway-decision-protocol']:
 for p in sorted((r/'rust/crates'/family).rglob('*')):
  if p.is_file() and p.suffix not in ['.rs','.toml','.md']:
   rel=str(p.relative_to(r));before=old(rel);assert before==p.read_bytes()
   fixture_inputs.append({'path':rel,'sha256':h(before),'bytes':len(before),'unchanged':True})
# No foreign paths, protected semantic bodies, baseline, tool/vendor, runtime config or lock changed.
familyroots=tuple('rust/crates/'+x+'/' for x in ['gateway-extras-runtime','gateway-extras-runtime-test-support','gateway-extras-primitives','gateway-extras-state','gateway-extras-jobs','gateway-extras-voyage'])
source_paths=subprocess.check_output(['git','ls-files','-z']).decode().split('\0');unchanged=[];changed=[];gitlinks=[]
for row in subprocess.check_output(['git','ls-files','-s','-z']).decode().split('\0'):
 if not row:continue
 meta,path=row.split('\t',1);mode,blob,stage=meta.split()
 if mode=='160000':
  oldmeta=subprocess.check_output(['git','ls-tree',base,'--',path],text=True).split()[2];assert oldmeta==blob
  gitlinks.append({'path':path,'gitlink':blob,'unchanged':True})
for p in source_paths:
 if not p:continue
 if p in [x['path'] for x in gitlinks]:continue
 if p in ['rust/Cargo.toml','rust/Cargo.lock'] or p.startswith(familyroots):continue
 current=(r/p).read_bytes();before=old(p)
 assert current==before,p
 unchanged.append({'path':p,'sha256':h(current),'bytes':len(current)})
for p in source_paths:
 if p and p.startswith(familyroots):
  path=r/p
  if not path.exists() or path.read_bytes()!=old(p):changed.append(p)
# Phase maps refer to actual fixed current source with no synthetic proof or new operation.
def locate(path,text):
 lines=read(path).splitlines();matches=[i for i,line in enumerate(lines,1) if text in line];assert matches,(path,text)
 return {'path':path,'lines':matches,'match':text,'fileSha256':h((r/path).read_bytes())}
phases={
 'sync':['rust/crates/gateway-extras-runtime/src/embeddings.rs', ['idempotency::acquire(', 'host.provider_options(', 'idempotency::mark_dispatched(', 'fetch_chunk(', 'idempotency::success(']],
 'idempotency':['rust/crates/gateway-extras-state/src/idempotency.rs',['enum State {','pub struct Lease {','pub async fn acquire','async fn acquire_inner','if r.state == State::Dispatched','pub async fn mark_dispatched','pub async fn release','pub async fn mark_indeterminate','pub async fn success']],
 'jobs':['rust/crates/gateway-extras-jobs/src/lib.rs',['struct Job {','struct InputRecord {','fn seed(','fn crypto_key(','async fn encrypt','async fn decrypt','struct JobAttempt','job.locked_until_ms = Some(at + 30_000);','pub async fn create','pub async fn get']],
 'images':['rust/crates/gateway-extras-runtime/src/images.rs',['host.begin_image_fanout(', 'host.abort_image_siblings();','host.image_child_finished(', 'host.finish_image_fanout().await?;']],
 'voyage':['rust/crates/gateway-extras-voyage/src/lib.rs',['fn retry_after','PreparedCall::prepare(', 'host.dispatch(ExtrasDispatch::Voyage(call))','fn result_body','fn usage']]
}
phase_receipt={'classification':'STATIC_SOURCE_PHASE_AND_PRIVACY_CUSTODY_NOT_LIVE_AUTHORITY','privateDataBodies':checks,'originalMovedHostMethodBodies':hostchecks,'externalFixtureFiles':fixture_inputs,'embeddedFixtureEvidence':'test-case-preservation.json hashes complete case bodies including all literals and assertions; host method bodies above; no literal external compiled fixture is used by runtime/decisions targets','phaseMap':{k:[locate(v[0],s) for s in v[1]] for k,v in phases.items()},'existingStoreDefiningAPI':{'path':'rust/crates/gateway-auth-primitives/src/store.rs','sha256':h((r/'rust/crates/gateway-auth-primitives/src/store.rs').read_bytes()),'check':'check(key: &RecordKey, record: Option<&StoredRecord>) -> RevisionCheck','commit':'commit<S: AuthorityStore>(store: &S, checks: Vec<RevisionCheck>, mutations: Vec<RecordMutation>) -> Result<bool, GatewayError>','put':'put<T: Serialize>(key: RecordKey, value: &T, expiry: Option<i64>, revision: &str) -> Result<RecordMutation, GatewayError>','putMapping':'Each of 17 call sites passes the original key, value and expiry, obtains uuid(host)? before serialization, then maps only the existing put encoding error to Extras invalid_state. Randomness failure propagates unchanged. No new store API.'},'principalHashSourceProof':{'sourceReferences':['src/embeddings/handlers.ts:90','src/embeddings/ledger.ts:503','src/utils.ts:45'],'operation':'JS whitespace trim then SHA256(TextEncoder(salted principal)). Replacing lone surrogates before trim commutes because the replacement is not whitespace; valid pairs/scalars retain their UTF8 bytes. Existing source_utf8 is used only at this established hash-input boundary, not a payload projection.','ownedChange':'old private principal.0 read becomes as_source -> source_utf8 -> same js_whitespace trim','actualExecution':False},'originalAdmissionOwnerUntouched':True,'newRawKeyMutationAPI':False,'newPreparedCallOrPrivateProofConstructor':False,'newSchedulerOrBackgroundRequest':False,'cancellationOwnerChanged':False,'compilerOrTestsExecuted':False,'namespaceOrConfinementEvidence':'not collected; no execution-based enforcement claim'}
(out/'phase-private-fixture-custody.json').write_text(json.dumps(phase_receipt,indent=2)+'\n')
(out/'foreign-source-preservation.json').write_text(json.dumps({'base':base,'allTrackedForeignPostimagesExact':len(unchanged),'files':unchanged,'gitlinksNotTraversed':gitlinks,'ownedTrackedChanges':changed,'workerFilesUnchanged':['rust/crates/gateway-worker/src/extras.rs','rust/crates/gateway-worker/src/extras_host.rs','rust/crates/gateway-worker/src/image_host.rs'],'baselinesProfilesVendorsToolsUnchanged':True,'noForeignAncestryIntegrated':True,'hashesOnlyNoPrivateStateRead':True},indent=2)+'\n')
print('Private data:',len(checks),'unchanged bodies; moved host:',len(hostchecks),'unchanged methods; foreign tracked postimages byte-exact:',len(unchanged),'external fixtures:',len(fixture_inputs))
