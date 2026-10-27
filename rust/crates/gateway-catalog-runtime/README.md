# Catalog runtime

`CatalogRuntime` owns request-local provider memo, coalesced discovery, credential binding/currentness, durable observation writes and cold inference catalog resolution. Its private memo and selected source collator are never exported as construction or write authority. A configured cold request joins the fixed refresh and rereads the current accepted snapshot; an existing empty snapshot remains authoritative.

Catalog responsibilities import their defining modules directly:

- `gateway-catalog-inputs` supplies named catalog reads, observation data codecs, finite Codex source reads and source model selection data.
- `gateway-catalog-admin::admin::CatalogAdmin` implements complete public/admin catalog operations for the concrete runtime.
- `gateway-catalog-versioned::VersionedCatalog` implements complete exact-client-version refresh, lease and publication operations for that runtime; its lease/context and publication helpers remain private.
- `gateway-catalog-control::control` owns complete paid observation/provenance publication operations; only authority can issue dispatch or spend permission.
- `gateway-catalog-provider` retains fixed host contracts and source-observed provider model projection.

The operation traits contain their actual source logic. They do not forward through legacy API facades or expose memo, secret, lease or proof constructors. Ordinary OpenAI model listings and rich exact-version Codex catalogs remain separate contracts; source UTF16, observed endpoint/reasoning metadata, collation, cancellation and publication fences are unchanged.

The conventional integration targets cover cache behavior, discovery, source data, lease/currentness fences, paid authority and authenticated configuration. Each target keeps its private fixture closure, and native owner proofs remain obtained through the existing real authentication/publication operations. Static structure does not establish actual Worker, native or deployment acceptance.
