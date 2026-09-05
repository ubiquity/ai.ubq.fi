// Wrapper: the implementation lives in the protected bootstrap package.
export * from "./bootstrap/revision-control.ts";
import { runRevisionControlCli } from "./bootstrap/revision-control.ts";
if (import.meta.main) await runRevisionControlCli();
