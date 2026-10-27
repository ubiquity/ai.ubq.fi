import {writeFileSync} from "node:fs";
import {resolve} from "node:path";



// Import the source-only module resolver before any source usage modules are loaded.
import {plugin} from "bun";
import {readFileSync} from "node:fs";

plugin({name:"frozen-source-only",setup(build){
 build.onResolve({filter:/^\.\/claude-code-quota\.ts$/},()=>({path:resolve(import.meta.dir,"source-sanitize.ts")}));
 build.onResolve({filter:/^\.\/claude-code\.ts$/},()=>({path:resolve(import.meta.dir,"source-constants.ts")}));
 build.onLoad({filter:/claude-code-quota\.ts$/},()=>({contents:readFileSync(resolve(import.meta.dir,"source-sanitize.ts"),"utf8"),loader:"ts"}));
 build.onLoad({filter:/provider\/claude-code\.ts$/},()=>({contents:readFileSync(resolve(import.meta.dir,"source-constants.ts"),"utf8"),loader:"ts"}));
}});

const {parseClaudeCodeUsagePayload}=await import("./src/provider/claude-code-usage.ts");
const {recordClaudeCapacityObservation,readClaudeCapacityHistory}=await import("./src/provider/claude-capacity-history.ts");
const {listClaudeUsageRollups,listClaudeCapacityRollups}=await import("./src/provider/claude-capacity-rollups.ts");
const rows=new Map<string,any>();let clock=7200000;let revision=0;
const compare=(a:any[],b:any[])=>{for(let i=0;i<Math.min(a.length,b.length);i++){if(a[i]!==b[i])return a[i]<b[i]?-1:1;}return a.length-b.length;};
const kv={
 async get(key:any[]){return structuredClone(rows.get(JSON.stringify(key))??{key,value:null,versionstamp:null});},
 atomic(){const checks:any[]=[];const writes:any[]=[];const op={check(e:any){checks.push(e);return op;},set(key:any[],value:any,opts:any={}){writes.push({key,value,expires_at_ms:opts.expireIn?clock+opts.expireIn:null});return op;},async commit(){if(checks.some(e=>(rows.get(JSON.stringify(e.key))?.versionstamp??null)!==e.versionstamp))return {ok:false};for(const w of writes)rows.set(JSON.stringify(w.key),{...structuredClone(w),versionstamp:String(++revision)});return {ok:true};}};return op;},
 async *list(range:any,options:any){let list=[...rows.values()].filter(r=>compare(r.key,range.start)>=0&&compare(r.key,range.end)<0&&(!r.expires_at_ms||r.expires_at_ms>clock)).sort((a,b)=>compare(a.key,b.key));if(options.reverse)list.reverse();for(const r of list.slice(0,options.limit))yield structuredClone(r);}
};
const originalNow=Date.now;Date.now=()=>clock;
const meter={kind:"future\ud800",group:"weekly",percent:121.5,resets_at:null,severity:"raw",is_active:true,scope:{model:{display_name:"model\udfff"}}};
const cases=[];
const payloads=[{limits:[meter,meter]},{limits:[{...meter,percent:-4},meter]},{limits:[]},{limits:[{...meter,resets_at:"1970-01-01T03:00:00Z"}]},{five_hour:{utilization:10,resets_at:null},limits:[]},{five_hour:{utilization:20,resets_at:null},limits:[]}];
for(let i=0;i<payloads.length;i++){
 clock=7200000+i*30000;
 const observation=parseClaudeCodeUsagePayload(payloads[i],clock)!;
 const event={v:1,observation_id:"fixture-"+i,account_id:"account",account_cohort_id:"cohort",credential_kind:"oauth",workspace_id:null,source_observed_at_ms:clock,sampled_at_ms:clock,response_status:200,state:i===4?"unavailable":"available",observation};
 await recordClaudeCapacityObservation(kv as any,event as any);
 cases.push({event,rows:[...rows.values()].map(({key,value,expires_at_ms})=>({key,value,expires_at_ms})),history:await readClaudeCapacityHistory(kv as any,clock),usage_rollups:await listClaudeUsageRollups(kv as any,0,clock),rollups:await listClaudeCapacityRollups(kv as any,0,clock)});
}
Date.now=originalNow;
writeFileSync(resolve(import.meta.dir,"../../../gateway-observability/tests/fixtures/claude-usage-history-oracle.json"),JSON.stringify({source_sha:"b59243d674567d418d1410f8390fc7fe47a08c92",cases},null,2)+"\n");
console.log(JSON.stringify({cases:cases.length,rows:rows.size}));
