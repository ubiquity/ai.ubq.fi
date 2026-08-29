/**
 * Task manifest loading and selection.
 *
 * Manifests live in benchmarks/tasks/*.json and are validated through the
 * shared contract in schemas.ts. Selection supports exact ids, `*` glob
 * patterns per segment, and `category:<name>` selectors, comma-separated.
 */

import { TaskCategory, TaskManifest, validateTaskManifest } from "./schemas.ts";

export class ManifestError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ManifestError";
  }
}

export function loadTasks(tasksDir: string): TaskManifest[] {
  const files: string[] = [];
  for (const entry of Deno.readDirSync(tasksDir)) {
    if (entry.isFile && entry.name.endsWith(".json")) files.push(entry.name);
  }
  files.sort();
  const tasks = files.map((name) => {
    const raw = Deno.readTextFileSync(`${tasksDir}/${name}`);
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch (err) {
      throw new ManifestError(`${name}: invalid JSON: ${(err as Error).message}`);
    }
    try {
      return validateTaskManifest(parsed);
    } catch (err) {
      throw new ManifestError(`${name}: ${(err as Error).message}`);
    }
  });
  const ids = new Set<string>();
  for (const t of tasks) {
    if (ids.has(t.id)) throw new ManifestError(`duplicate task id ${t.id}`);
    ids.add(t.id);
  }
  return tasks;
}

/** Simple glob matcher for `*` and `?` (no `**`; segment-aligned). */
function globMatches(pattern: string, value: string): boolean {
  if (pattern === "*") return true;
  const re = new RegExp(
    "^" +
      pattern.split("").map((c) => (c === "*" ? ".*" : c === "?" ? "." : c.replace(/[.+^${}()|[\]\\]/g, "\\$&"))).join(
        "",
      ) + "$",
  );
  return re.test(value);
}

/**
 * Select tasks with comma-separated selectors: exact `nav-001`, glob
 * `code-*`, `*`, or `category:navigation`. Empty/`*` selects all tasks.
 */
export function selectTasks(tasks: TaskManifest[], selectors: string[]): TaskManifest[] {
  if (selectors.length === 0) return tasks;
  const chosen = new Set<string>();
  const unknown: string[] = [];
  for (const sel of selectors) {
    if (sel.startsWith("category:")) {
      const cat = sel.slice("category:".length) as TaskCategory;
      if (!tasks.some((t) => t.category === cat)) {
        unknown.push(sel);
        continue;
      }
      for (const t of tasks) if (t.category === cat) chosen.add(t.id);
      continue;
    }
    const matched = tasks.filter((t) => globMatches(sel, t.id));
    if (matched.length === 0) unknown.push(sel);
    for (const t of matched) chosen.add(t.id);
  }
  if (unknown.length > 0) {
    const known = tasks.map((t) => t.id);
    throw new ManifestError(`no tasks matched: ${unknown.join(", ")}; known ids: ${known.join(", ")}`);
  }
  return tasks.filter((t) => chosen.has(t.id));
}

/** Progressively restricted display name of a derived task family. */
export function taskFamily(task: TaskManifest): string {
  return task.id.replace(/-\d+$/, "");
}
