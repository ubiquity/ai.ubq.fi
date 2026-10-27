import { plugin } from "bun";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";

// Exact frozen TS is data for the source oracle, outside the application graph.
const root = resolve(import.meta.dir);
plugin({
  name: "frozen-claude-source-inputs",
  setup(build) {
    build.onResolve({ filter: /^\.{1,2}\// }, ({ path, importer }) => {
      const candidate = resolve(dirname(importer), path);
      if (!candidate.startsWith(root + "/")) return;
      if (path === "./claude-code-quota.ts") return { path: resolve(root, "source-sanitize.ts.txt") };
      if (path === "./claude-code.ts") return { path: resolve(root, "source-constants.ts.txt") };
      if (existsSync(candidate + ".txt")) return { path: candidate + ".txt" };
    });
    build.onLoad({ filter: /\.ts\.txt$/ }, ({ path }) => ({ contents: readFileSync(path, "utf8"), loader: "ts" }));
  },
});
