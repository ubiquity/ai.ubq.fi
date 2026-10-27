from pathlib import Path
import re,subprocess,json,hashlib,tomllib,ast
r=Path.cwd();out=r/'handoff-evidence/extras-runtime-family/source-family-v1';base='7694a49787c7379180cb0e268b8b03fac093d879';cr=r/'rust/crates'
def gitshow(p):return subprocess.check_output(['git','show',base+':'+p],text=True)
def sha(s):return hashlib.sha256(s.encode()).hexdigest()
# Token inventory only, not a Rust syntax/structural gate. Keeps full literal bytes.
def tokens(s):
 result=[];i=0
 while i<len(s):
  if s[i].isspace():i+=1;continue
  if s.startswith('//',i):
   j=s.find('\n',i);i=len(s) if j<0 else j+1;continue
  if s.startswith('/*',i):
   depth=1;i+=2
   while depth:
    if s.startswith('/*',i):depth+=1;i+=2
    elif s.startswith('*/',i):depth-=1;i+=2
    else:i+=1
   continue
  m=re.match(r'(?:b|c)?r(#{0,255})"',s[i:])
  if m:
   end='"'+m[1];j=s.index(end,i+len(m[0]))+len(end);result.append(s[i:j]);i=j;continue
  m=re.match(r"'(?:\\(?:u\{[0-9a-fA-F_]+\}|x[0-9a-fA-F]{2}|.)|[^'\\\n])'",s[i:])
  if m:result.append(m[0]);i+=len(m[0]);continue
  m=re.match(r'(?:b|c)?"',s[i:])
  if m:
   j=i+len(m[0])
   while j<len(s):
    if s[j]=='\\':j+=2
    elif s[j]=='"':j+=1;break
    else:j+=1
   result.append(s[i:j]);i=j;continue
  m=re.match(r'(?:r#)?[A-Za-z_][A-Za-z_0-9]*|[0-9][A-Za-z_0-9]*|::|->|=>|==|!=|<=|>=|&&|\|\||\.\.=|\.\.|.',s[i:])
  assert m,s[i:i+80];result.append(m[0]);i+=len(m[0])
 return result
def functions(s, helper_format=False):
 t=tokens(s);found=[]
 for i,v in enumerate(t):
  if v!='fn' or not re.match('^[A-Za-z_]',t[i+1]):continue
  j=i+2
  while j<len(t) and t[j] not in ['{',';']:j+=1
  if j==len(t) or t[j]==';':continue
  end=j+1;depth=1
  while depth:
   depth+=(t[end]=='{')-(t[end]=='}');end+=1
  body=t[j:end]
  if helper_format: body=[v for k,v in enumerate(body) if not (v==',' and k>0 and k+1<len(body) and body[k-1]=='?' and body[k+1] in [')',']'])]
  found.append({'name':t[i+1],'bodySha256':sha(' '.join(body)),'body':' '.join(body)})
 return found
# Reuse only bounded source-text conversion functions already used for these moves.
source=Path('/tmp/ex-family-extract-v1.py').read_text();tree=ast.parse(source);scope={'re':re}
for n in tree.body:
 if isinstance(n,ast.FunctionDef) and n.name in ['span','puts','port']:exec(compile(ast.Module(body=[n],type_ignores=[]),'<source-converter>','exec'),scope)
puts=scope['puts'];port=scope['port']
# Exact expected nominal and pre-existing-helper API changes, no arbitrary normalization.
def expected(s):
 s=port(s)
 s=s.replace('use gateway_auth::{crypto::Secret, types::AuthMethod};','use gateway_auth_primitives::crypto::Secret;\nuse gateway_auth_contracts::types::AuthMethod;')
 s=s.replace('crate::store::source_utf8','gateway_extras_primitives::value::source_utf8')
 s=s.replace('let principal = request.context.principal.0.trim_matches(js_whitespace);','let principal = source_utf8(request.context.principal.as_source());\n        let principal = principal.trim_matches(js_whitespace);')
 s=s.replace('jobs::create(', 'create(').replace('jobs::get(', 'get(').replace('store::js_whitespace','js_whitespace')
 return s
maps={
 'cache.rs':['gateway-extras-state/src/cache.rs'],
 'idempotency.rs':['gateway-extras-state/src/idempotency.rs'],
 'jobs.rs':['gateway-extras-jobs/src/lib.rs'],
 'embeddings.rs':['gateway-extras-voyage/src/lib.rs','gateway-extras-runtime/src/embeddings.rs'],
 'images.rs':['gateway-extras-runtime/src/images.rs'],
 'lib.rs':['gateway-extras-runtime/src/lib.rs'],
 'store.rs':['gateway-extras-primitives/src/value.rs'],
}
comp=[];failures=[]
for old,news in maps.items():
 orig=gitshow('rust/crates/gateway-extras-runtime/src/'+old);exp=expected(orig)
 if old in ['cache.rs','idempotency.rs','jobs.rs']:exp,_=puts(exp)
 before=functions(orig);exps=functions(exp,True)
 after=[dict(x,path='rust/crates/'+p) for p in news for x in functions((cr/p).read_text(),True)]
 for original,want in zip(before,exps):
  if old=='store.rs' and original['name'] in ['put','check','commit']:
   comp.append({'from':old,'name':original['name'],'originalBodySha256':original['bodySha256'],'disposition':'duplicate private helper removed; consumers use existing gateway_auth_primitives::store definition directly','definingPath':'rust/crates/gateway-auth-primitives/src/store.rs'});continue
  matches=[x for x in after if x['name']==want['name'] and x['bodySha256']==want['bodySha256']]
  item={'from':old,'name':original['name'],'originalBodySha256':original['bodySha256'],'expectedBodySha256':want['bodySha256'],'matches':[{'path':m['path'],'bodySha256':m['bodySha256']} for m in matches],'sameBodyExceptListedNominalAndHelperPort':len(matches)==1}
  comp.append(item)
  if len(matches)!=1:failures.append(item)
(out/'source-body-moves.json').write_text(json.dumps({'classification':'STATIC_TOKEN_BODY_COMPARISON_NOT_RUST_PARSER_OR_EXECUTION','comparisonOnlyFormattingNormalization':'Ignore rustfmt-added trailing comma after ? in generated call/list arguments only; this is not syntax or behavioral execution proof','transformations':['bc8 Source/Auth defining paths','17 put(host,key,value,expiry) sites -> existing auth put(key,value,expiry,&uuid(host)?).map_err(|_| invalid_state()); same source value/revision/expiry/error order','private PrincipalId projection -> existing TextEncoder hash input via as_source and source_utf8 before JS whitespace trim','runtime direct jobs create/get and primitive whitespace imports'], 'items':comp,'failures':failures},indent=2)+'\n')
print('Production function bodies:',len(comp),'compared;',len(failures),'mismatches',[(x['from'],x['name']) for x in failures])
# Tests keep names, control flow, assertions and literals, with defining-path-only ports.
cases=[]
for name in ['runtime','decisions']:
 p='rust/crates/gateway-extras-runtime/tests/'+name+'.rs';orig=gitshow(p);curr=(r/p).read_text()
 names=re.findall(r'#\[test\]\s*fn\s+(\w+)',orig);newnames=re.findall(r'#\[test\]\s*fn\s+(\w+)',curr)
 oldf=functions(orig);newf=functions(curr)
 for n in names:
  a=next(x for x in oldf if x['name']==n);b=next(x for x in newf if x['name']==n)
  want=next(x for x in functions(port(orig)) if x['name']==n)
  cases.append({'target':name,'name':n,'originalBodySha256':a['bodySha256'],'currentBodySha256':b['bodySha256'],'exactBody':a['bodySha256']==b['bodySha256'],'nominalOnlyBodyPreserved':want['bodySha256']==b['bodySha256']})
 assert names==newnames,(name,'test names/order')
 assert all(x['nominalOnlyBodyPreserved'] for x in cases),'test body drift'
 queued=next((x for x in newf if x['name']=='queued_job'),None)
 if queued:assert next(x for x in oldf if x['name']=='queued_job')['bodySha256']==queued['bodySha256']
(out/'test-case-preservation.json').write_text(json.dumps({'testsExecuted':False,'cases':cases,'count':len(cases),'runtime':sum(x['target']=='runtime' for x in cases),'decisions':sum(x['target']=='decisions' for x in cases),'allBodiesNominalOnlyPreserved':True,'queued_jobBodyExact':True},indent=2)+'\n')
print('Test names/order and bodies:',len(cases),'preserved;',sum(not x['exactBody'] for x in cases),'nominal-only path change; queued_job exact')
# Existing external dependencies and every foreign first-party lock block remain byte-exact.
lockold=gitshow('rust/Cargo.lock');locknew=(r/'rust/Cargo.lock').read_text()
def lockblocks(s):return {(tomllib.loads('[[package]]\n'+b)['package'][0]['name'],tomllib.loads('[[package]]\n'+b)['package'][0]['version'],tomllib.loads('[[package]]\n'+b)['package'][0].get('source','')):b for b in s.split('[[package]]\n')[1:]}
a=lockblocks(lockold);b=lockblocks(locknew)
allowed={'gateway-extras-runtime','gateway-extras-runtime-test-support','gateway-extras-primitives','gateway-extras-state','gateway-extras-jobs','gateway-extras-voyage'}
foreign=[k for k in set(a)|set(b) if k[0] not in allowed];bad=[k for k in foreign if a.get(k)!=b.get(k)]
assert not bad,bad
rootold=tomllib.loads(gitshow('rust/Cargo.toml'));rootnew=tomllib.loads((r/'rust/Cargo.toml').read_text())
newowners=allowed-{'gateway-extras-runtime','gateway-extras-runtime-test-support'}
for c in newowners:
 rootnew['workspace']['members'].remove('crates/'+c);del rootnew['workspace']['dependencies'][c]
assert rootold==rootnew,'root workspace semantics drift'
(out/'manifest-lock-preservation.json').write_text(json.dumps({'lockNotRegenerated':True,'externalAndForeignBlocksByteExact':len(foreign),'failures':bad,'allowedChangedOrNewBlocks':sorted(allowed),'workspaceOnlyNewOwnerRegistration':True,'profilesUnchanged':True,'newOwnerManifests':[{'crate':c,'sha256':sha((cr/c/'Cargo.toml').read_text())} for c in sorted(allowed)]},indent=2)+'\n')
print('Foreign/external lock blocks byte-exact:',len(foreign),'; root-only new owner registration')
# New production family DAG, no dev fixture edge in production.
owned=sorted(allowed);graph={};file_inventory=[];counts={};public={}
for c in owned:
 manifest=tomllib.loads((cr/c/'Cargo.toml').read_text());assert manifest['lints']['workspace']
 graph[c]=sorted(k for k in manifest.get('dependencies',{}) if k.startswith('gateway-'))
 files=sorted((cr/c/'src').rglob('*.rs'));counts[c]=sum(len(p.read_bytes().splitlines()) for p in files);assert counts[c]<=1000
 for p in files:
  txt=p.read_text();t=tokens(txt)
  assert not re.search(r'\bpub(?:\s*\([^)]*\))?\s+use\b|#\s*\[\s*(?:path|allow|expect)\b|\binclude\s*!', ' '.join(t)),p
  file_inventory.append({'path':str(p.relative_to(r)),'bytes':p.stat().st_size,'sha256':sha(txt),'lines':len(txt.splitlines())})
  public[str(p.relative_to(r))]=re.findall(r'^pub (?:async )?(?:fn|struct|enum|const|mod) .+',txt,re.M)
 for d in graph[c]:assert c=='gateway-extras-runtime' or d!='gateway-extras-runtime','runtime dependency backedge'
 assert c=='gateway-extras-runtime-test-support' or 'gateway-extras-runtime-test-support' not in graph[c]
# Full declared workspace static graph includes predecessor cycles? Only test acyclic for new/owned nodes.
visited=set();active=set();order=[]
def visit(c):
 if c not in graph or c in visited:return
 assert c not in active,c
 active.add(c)
 for d in graph[c]:visit(d)
 active.remove(c);visited.add(c);order.append(c)
for c in graph:visit(c)
targets=[]
for p in sorted((cr/'gateway-extras-runtime/tests').glob('*.rs')):
 # No conventional sibling modules remain; literal include inputs none in these targets.
 text=p.read_text();assert not re.search(r'^\s*mod\s+\w+\s*;',text,re.M)
 lines=len(text.splitlines());assert lines<=1000
 targets.append({'target':p.stem,'files':[str(p.relative_to(r))],'physicalLines':lines,'testCount':len(re.findall(r'#\[test\]',text)),'literalCompiledFixtures':re.findall(r'include_(?:str|bytes)!\s*\(\s*"([^"]+)"',text)})
(out/'dag-api-lines.json').write_text(json.dumps({'classification':'STATIC_DECLARED_SOURCE_NOT_COMPILER_METADATA','graph':graph,'ownedTopologicalOrder':order,'counts':counts,'files':file_inventory,'publicDeclarations':public,'testTargets':targets,'noProductionFixtureDependency':True,'noOwnedReexportsPathIncludeSuppressionsByTokenInventory':True,'newRawKeyMutationAPI':False},indent=2)+'\n')
print('Owned production counts:',counts,'; target lines:',[(x['target'],x['physicalLines']) for x in targets])
