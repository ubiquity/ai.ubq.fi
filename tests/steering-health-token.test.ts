import { test } from "bun:test";
import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { readToken } from "../scripts/steering-health.ts";

const withEnvFixture = async (text: string, check: (contents: string) => void): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), "uos-steering-token-test-"));
  try {
    const path = join(directory, "fixture.env");
    await writeFile(path, text, { mode: 0o600 });
    check(await readFile(path, "utf8"));
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
};

for (const [name, value] of [
  ["single token", "fixture-first"],
  ["single-quoted token", "'fixture-first'"],
  ["double-quoted token", '"fixture-first"'],
  ["inline comment", "fixture-first # ignored"],
  ["unquoted comma list", "fixture-first,fixture-second"],
  ["single-quoted comma list", "'fixture-first,fixture-second'"],
  ["double-quoted comma list", '"fixture-first,fixture-second"'],
  ["single-quoted newline list", "'fixture-first\nfixture-second'"],
  ["double-quoted newline list", '"fixture-first\nfixture-second"'],
  ["escaped newline list", '"fixture-first\\nfixture-second"'],
  ["empty entries and whitespace", '" , \n  fixture-first , ,\n fixture-second,\n"'],
  ["CRLF list", '" ,\r\n  fixture-first\r\nfixture-second\r\n"'],
]) {
  test(`steering token reader selects first configured token: ${name}`, async () => {
    await withEnvFixture(`# unrelated assignment\nOTHER_TOKEN=fixture-decoy\nexport UOS_AI_TOKEN=${value}\n`, (text) => {
      assert.ok(readToken(text) === "fixture-first", "Probe must select the first non-empty configured token");
    });
  });
}

for (const [name, text, error] of [
  ["missing assignment", "OTHER_TOKEN=fixture-decoy\n", "Repository-root .env lacks UOS_AI_TOKEN"],
  ["commented assignment", "# UOS_AI_TOKEN=fixture-decoy\n", "Repository-root .env lacks UOS_AI_TOKEN"],
  ["empty value", "UOS_AI_TOKEN=\n", "Repository-root UOS_AI_TOKEN is empty"],
  ["empty quoted value", 'UOS_AI_TOKEN=""\n', "Repository-root UOS_AI_TOKEN is empty"],
  ["only separators", 'UOS_AI_TOKEN=" ,\n , \r\n"\n', "Repository-root UOS_AI_TOKEN is empty"],
]) {
  test(`steering token reader rejects ${name} without exposing values`, async () => {
    await withEnvFixture(text, (contents) => {
      assert.throws(() => readToken(contents), { message: error });
    });
  });
}
