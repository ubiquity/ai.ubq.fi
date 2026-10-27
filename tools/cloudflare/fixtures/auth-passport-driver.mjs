// Auth-only test driver. The local source rows live in the actual compiled GatewayAuthority.
import { DurableObject } from "cloudflare:workers";
import { authPassportControl, authPassportUpstream } from "./auth-passport-upstream.mjs";

export class AuthPassportControl extends DurableObject {
  async fetch(request) {
    if (new URL(request.url).hostname === "passport.ubq.fi") return authPassportUpstream(request, this.ctx.storage);
    return (await authPassportControl(request, this.ctx.storage)) ?? new Response(null, { status: 404 });
  }
}
const control = (env) => env.CONTROL.getByName("auth-passport-control-v2");
const encoder = new TextEncoder();
const tuplePart = (value) => "02" + Array.from(encoder.encode(value), (byte) => byte.toString(16).padStart(2, "0")).join("") + "00";
const sessionPrefix = "tuple-v1:" + ["uos_ai", "auth", "passport", "sessions"].map(tuplePart).join("") + "02";
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.hostname === "passport.ubq.fi" || url.pathname === "/auth-passport/control") return control(env).fetch(request);
    if (url.pathname !== "/auth-passport/read" || request.method !== "POST") return new Response(null, { status: 404 });
    const body = await request.json();
    // Observational access is restricted to this fixture's exact Passport source family.
    if (typeof body.key !== "string" || !body.key.startsWith(sessionPrefix) || !/^(?:3[0-9]|6[1-6]){64}00$/u.test(body.key.slice(sessionPrefix.length))) return new Response(null, { status: 400 });
    const authority = env.AUTHORITY.get(env.AUTHORITY.idFromName("gateway-authority-v1"));
    return authority.fetch(new Request("https://authority.internal/v1/store", {
      method: "POST",
      headers: { "content-type": "application/json", "x-uos-authority-wire-version": "1" },
      body: JSON.stringify({ operation: "read", key: body.key }),
    }));
  },
};
