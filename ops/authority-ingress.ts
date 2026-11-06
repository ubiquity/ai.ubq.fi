const blockEnd = (text: string, start: number): number => {
  const open = text.indexOf("{", start);
  if (open < 0) throw new Error("Expected bounded ingress block");
  let depth = 1;
  for (let index = open + 1; index < text.length; index++) {
    if (text[index] === "{") depth++;
    if (text[index] === "}" && --depth === 0) return index + 1;
  }
  throw new Error("Incomplete ingress block");
};
const replaceProxy = (text: string, token: string, replacement: string): string => {
  const start = text.indexOf(token);
  if (start < 0 || text.includes(token, start + token.length)) throw new Error("Ambiguous stale AI ingress");
  const end = blockEnd(text, start);
  if (!text.slice(start, end).includes("127.0.0.1:7999")) throw new Error("Unexpected AI ingress target");
  return text.slice(0, start) + replacement + text.slice(end);
};
const splitSite = (text: string, name: string): { prefix: string; site: string } => {
  const lines = text.split("\n");
  const first = lines.findIndex((line) => line.trim() !== "" && !line.trim().startsWith("#"));
  if (first < 0 || !lines[first].trim().startsWith(name + " {")) throw new Error("Unsupported AI ingress site");
  return { prefix: lines.slice(0, first).join("\n"), site: lines.slice(first).join("\n") };
};
/** Pure controller composition only; never writes or reloads live Caddy. */
export const composeCloudflareAuthorityIngress = (publicAi: string, origin: string): { publicAi: string; origin: string } => {
  const publicInput = splitSite(publicAi, "ai.ubq.fi");
  const originInput = splitSite(origin, "origin.ubq.fi");
  publicAi = publicInput.site;
  origin = originInput.site;
  if (origin.includes("@uos_authority_native")) throw new Error("Unsupported AI ingress composition");
  const publicResult = replaceProxy(publicAi, "reverse_proxy 127.0.0.1:7999", 'respond "Cloudflare authority required" 503');
  const originResult = replaceProxy(origin, "handle @ai {", 'handle @ai { respond "Cloudflare authority required" 503 }');
  const open = originResult.indexOf("{"),
    end = blockEnd(originResult, 0);
  if (originResult.slice(end).trim()) throw new Error("Unexpected additional origin sites");
  const body = originResult.slice(open + 1, end - 1);
  const lines = body.split("\n");
  const isDefinition = (line: string): boolean => /^@[A-Za-z0-9_-]+ header /.test(line.trim());
  const definitions = lines.filter(isDefinition);
  if (!definitions.some((line) => line.includes("@ai header X-Forwarded-Host ai.ubq.fi"))) throw new Error("Missing AI host selector");
  const handles = lines.filter((line) => !isDefinition(line)).join("\n");
  return {
    publicAi: publicInput.prefix + "\n" + publicResult,
    origin:
      originInput.prefix +
      "\norigin.ubq.fi {\n" +
      definitions.join("\n") +
      "\n@uos_authority_native {\n method POST\n path /v1/native/rpc /v1/native/control /v1/native/follow\n}\nroute {\nhandle @uos_authority_native {\n reverse_proxy 127.0.0.1:7990\n}\n" +
      handles +
      "\n}\n}\n",
  };
};
