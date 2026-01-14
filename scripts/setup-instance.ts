import { deriveRsaPublicKeyPemFromPrivateKey, normalizeMultilineSecret } from "../../../src/github/utils/rsa.ts";

async function main() {
  const appId = Deno.env.get("APP_ID");
  const privateKeyRaw = Deno.env.get("APP_PRIVATE_KEY");
  const aiToken = Deno.env.get("UOS_AI_TOKEN");
  const aiUrl = Deno.env.get("UOS_AI_URL") ?? "https://ai.ubq.fi";
  const owner = Deno.env.get("UOS_OWNER") ?? "unknown";

  if (!appId || !privateKeyRaw || !aiToken) {
    console.error("Missing environment variables: APP_ID, APP_PRIVATE_KEY, UOS_AI_TOKEN are required.");
    Deno.exit(1);
  }

  const privateKey = normalizeMultilineSecret(privateKeyRaw);
  console.log(`Deriving public key for App ID: ${appId}...`);

  let publicKeyPem: string;
  try {
    publicKeyPem = await deriveRsaPublicKeyPemFromPrivateKey(privateKey);
  } catch (err) {
    console.error("Failed to derive public key:", err);
    Deno.exit(1);
  }

  console.log(`Uploading public key to ${aiUrl}...`);

  const res = await fetch(`${aiUrl}/admin/kernel-pubkeys`, {
    method: "POST",
    headers: {
      "Authorization": `Bearer ${aiToken}`,
      "Content-Type": "application/json",
      "Accept": "application/json",
    },
    body: JSON.stringify({
      app_id: Number(appId),
      pem: publicKeyPem,
      owner: owner,
    }),
  });

  const text = await res.text();
  if (res.ok) {
    console.log("Success! Public key registered.");
    console.log(text);
  } else {
    console.error(`Failed to register public key (status ${res.status}):`);
    console.error(text);
    Deno.exit(1);
  }
}

if (import.meta.main) {
  main();
}
