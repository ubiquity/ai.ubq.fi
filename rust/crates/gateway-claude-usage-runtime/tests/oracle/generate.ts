import {plugin} from "bun";
import {readFileSync,writeFileSync} from "node:fs";
import {resolve} from "node:path";
plugin({name:"frozen-source-only",setup(build){
 build.onResolve({filter:/^\.\/claude-code-quota\.ts$/},()=>({path:resolve(import.meta.dir,"source-sanitize.ts")}));
 build.onResolve({filter:/^\.\/claude-code\.ts$/},()=>({path:resolve(import.meta.dir,"source-constants.ts")}));
 build.onLoad({filter:/claude-code-quota\.ts$/},()=>({contents:readFileSync(resolve(import.meta.dir,"source-sanitize.ts"),"utf8"),loader:"ts"}));
 build.onLoad({filter:/provider\/claude-code\.ts$/},()=>({contents:readFileSync(resolve(import.meta.dir,"source-constants.ts"),"utf8"),loader:"ts"}));
}});
const schema=await import("./src/provider/claude-code-usage-schema.ts");
const usage=await import("./src/provider/claude-code-usage.ts");
const at=1800000000000;
const rows: {name:string;input:string}[]=[];
const add=(name:string,value:unknown)=>rows.push({name,input:JSON.stringify(value)});
for(const name of schema.CLAUDE_USAGE_LEGACY_WINDOWS){for(const n of [null,0,-5,12.5,100,121.5])add(`${name}:${n}`,{[name]:{utilization:n,resets_at:n===0?null:"2026-10-25T23:00:00.123456+00:00"},vendor:{opaque:"\ud800"}});add(`${name}:null`,{[name]:null});}
const row={kind:"future\ud800",group:"weekly",percent:121.5,resets_at:"2026-11-01T18:00:00Z",severity:"vendor_future",is_active:true,scope:{surface:{display_name:"OAuth \udfff",vendor:7},model:{display_name:"Future"},"2":true,"1":null,vendor:"retained"}};
for(const scope of [undefined,null,{},[],{model:null},{surface:{display_name:""}},{model:{display_name:"\ud800"},surface:null}]){add(`scope:${JSON.stringify(scope)}`,{limits:[{...row,scope},{...row,scope}],five_hour:{utilization:42,resets_at:null}});}
add("dynamic-order",{limits:[row,{...row,kind:"session",percent:-4,is_active:false},row],seven_day:{utilization:10,resets_at:null},vendor:{retained:true}});
add("empty-precedence",{limits:[],seven_day:{utilization:14,resets_at:null}});
add("null-fallback",{limits:null,five_hour:{utilization:105,resets_at:"invalid"}});
add("credits",{extra_usage:{is_enabled:false,monthly_limit:null,used_credits:12,utilization:null,currency:"USD",disabled_reason:null},cinder_cove:{utilization:110,resets_at:null},wattle_ember:{utilization:null,resets_at:"invalid",periods:[{starts_at:"start",ends_at:"end",unknown:2}],label:"\ud800",limit_dollars:null,used_dollars:2,remaining_dollars:-2},unknown:[null,"opaque"]});
for(const bad of [{},{foo:null},{five_hour:{}},{seven_day:{utilization:1}},{limits:[{...row,percent:"1"}]},{limits:[{...row,scope:{model:{}}}]},{extra_usage:{is_enabled:true}},{wattle_ember:{utilization:1,resets_at:null,periods:[{}]}}])add("invalid",bad);
rows.push({name:"duplicate-numeric-unknown",input:'{"five_hour":{"utilization":1,"utilization":-0,"resets_at":null},"unknown":{"z":1e400,"2":"second","1":"first","\\ud800":-0,"z":2}}'});
const dates:Record<string,number|null>={};
const inspect=(value:unknown)=>{if(typeof value==="string"){const at=Date.parse(value);dates[value]=Number.isSafeInteger(at)&&at>0?at:null;}else if(Array.isArray(value)){for(const v of value)inspect(v);}else if(value&&typeof value==="object"){for(const v of Object.values(value))inspect(v);}};
const cases=rows.map(r=>{const payload=JSON.parse(r.input);inspect(payload);return {...r,snapshot:schema.parseClaudeCodeUsageSnapshot(payload,at),observation:usage.parseClaudeCodeUsagePayload(payload,at)}});
const errors=[];
for(const [status,body,retry] of [[403,"",null],[401,'{"error":{"message":"bad token"}}',null],[401,'{"message":"insufficient scope"}',null],[429,'{"message":"wait"}',"1.25"],[429,"wait","  "],[429,"wait","0x10"],[429,"wait","Wed, 20 Jan 2027 00:00:00 GMT"],[500,"a\t \n b",null],[302,"redirect",null],[418,'{"error":{"message":"  diagnostic\\ud800  "}}',null],[200,"bad json",null],[200,"{}",null],[400,"x".repeat(4090)+"\ud800"+"tail".repeat(20),null]] as const){
 inspect(retry);
 const result=await usage.fetchClaudeCodeUsage({kind:"oauth",secret:"public-fixture-only"},{now:()=>at,fetcher:async()=>new Response(body,{status,headers:retry===null?{}:{"retry-after":retry}})});
 errors.push({status,body:await new Response(body).text(),retry:retry===null?null:new Headers({"retry-after":retry}).get("retry-after"),result});
}
const out={source_sha:"b59243d674567d418d1410f8390fc7fe47a08c92",at,dates:Object.entries(dates),cases,errors};
writeFileSync(resolve(import.meta.dir,"../fixtures/source-oracle.json"),JSON.stringify(out,null,2)+"\n");
console.log(JSON.stringify({cases:cases.length,errors:errors.length,source:out.source_sha}));
