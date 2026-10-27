import {plugin} from "bun";
import {readFileSync,writeFileSync} from "node:fs";
import {resolve} from "node:path";
plugin({name:"frozen-source-only",setup(build){
 build.onResolve({filter:/^\.\/claude-code-quota\.ts$/},()=>({path:resolve(import.meta.dir,"source-sanitize.ts")}));
 build.onResolve({filter:/^\.\/claude-code\.ts$/},()=>({path:resolve(import.meta.dir,"source-constants.ts")}));
 build.onLoad({filter:/claude-code-quota\.ts$/},()=>({contents:readFileSync(resolve(import.meta.dir,"source-sanitize.ts"),"utf8"),loader:"ts"}));
 build.onLoad({filter:/provider\/claude-code\.ts$/},()=>({contents:readFileSync(resolve(import.meta.dir,"source-constants.ts"),"utf8"),loader:"ts"}));
}});

const {fetchClaudeCodeUsage}=await import("./src/provider/claude-code-usage.ts");
const timeouts:number[]=[];const original=AbortSignal.timeout;AbortSignal.timeout=(ms:number)=>{timeouts.push(ms);return original.call(AbortSignal,ms);};
const requests:unknown[]=[];
const fetcher:typeof fetch=async(input,init)=>{requests.push({url:String(input),method:init?.method,redirect:init?.redirect,headers:[...new Headers(init?.headers)],aborted:init?.signal?.aborted});return new Response('{"limits":[]}');};
await fetchClaudeCodeUsage({kind:"oauth",secret:"public-fixture-only"},{fetcher,now:()=>1800000000000});
const before=requests.length;
const unsupported=await fetchClaudeCodeUsage({kind:"api_key",secret:"public-fixture-only"},{fetcher,now:()=>1800000000000});
AbortSignal.timeout=original;
writeFileSync(resolve(import.meta.dir,"../fixtures/wire-oracle.json"),JSON.stringify({source_sha:"b59243d674567d418d1410f8390fc7fe47a08c92",timeouts,requests,unsupported,unsupported_requests:requests.length-before},null,2)+"\n");
console.log(JSON.stringify({requests:requests.length,timeouts,unsupported_requests:requests.length-before}));
