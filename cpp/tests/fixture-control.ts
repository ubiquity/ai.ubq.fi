import assert from "node:assert/strict";
import { dlopen, FFIType, ptr } from "bun:ffi";
import { createReadStream, fstatSync, readlinkSync, writeSync } from "node:fs";
import { createConformanceJournal, type ConformanceEvent } from "../../tests/helpers/responses-conformance-client.ts";

const libc = dlopen("libc.so.6", {
  pipe2: { args: [FFIType.ptr, FFIType.i32], returns: FFIType.i32 },
  fcntl: { args: [FFIType.i32, FFIType.i32, FFIType.i32], returns: FFIType.i32 },
  close: { args: [FFIType.i32], returns: FFIType.i32 },
});
const makePipe = () => {
  const descriptors = new Int32Array(2);
  assert.equal(libc.symbols.pipe2(ptr(descriptors), 0x80000), 0, "Create actual anonymous close-on-exec pipes");
  for (const fd of descriptors) {
    assert.equal(libc.symbols.fcntl(fd, 1031, 4096), 4096, "Bound the owned pipe capacity");
    assert.equal(fstatSync(fd).isFIFO(), true);
    assert.match(readlinkSync(`/proc/self/fd/${fd}`), /^pipe:\[/);
  }
  return { read: descriptors[0], write: descriptors[1] };
};

/** No resident bridge: the actual kernel pipe ends are inherited by the candidate. */
export const createFixtureControl = () => {
  const command = makePipe();
  const reply = makePipe();
  const journal = createConformanceJournal();
  const reports = [command.read, command.write, reply.read, reply.write].map((fd) => ({
    fd,
    target: readlinkSync(`/proc/self/fd/${fd}`),
    uid: fstatSync(fd).uid,
    fifo: fstatSync(fd).isFIFO(),
    flags: libc.symbols.fcntl(fd, 3, 0),
    capacity: libc.symbols.fcntl(fd, 1032, 0),
  }));
  let sequence = 0;
  let pending = Buffer.alloc(0);
  let started = false;
  let closed = false;
  const input = createReadStream("", { fd: reply.read, autoClose: true });
  input.on("data", (chunk: Buffer | string) => {
    pending = Buffer.concat([pending, Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk)]);
    while (pending.length >= 4) {
      const size = pending.readUInt32BE(0);
      assert.ok(size > 0 && size <= 4092);
      if (pending.length < size + 4) break;
      const value: unknown = JSON.parse(pending.subarray(4, size + 4).toString());
      assert.ok(typeof value === "object" && value !== null && !Array.isArray(value));
      journal.add("reply", value as ConformanceEvent);
      pending = pending.subarray(size + 4);
    }
    assert.ok(pending.length <= 4096);
  });
  input.on("error", (error) => {
    journal.add("pipe_error", { message: error.message });
  });
  return {
    reports,
    commandRead: command.read,
    replyWrite: reply.write,
    config: { control_read_fd: 3, control_write_fd: 4 },
    started: (): void => {
      assert.equal(started, false);
      started = true;
      assert.equal(libc.symbols.close(command.read), 0);
      assert.equal(libc.symbols.close(reply.write), 0);
    },
    send: async (op: string, fields: ConformanceEvent = {}): Promise<ConformanceEvent> => {
      const id = ++sequence;
      const body = Buffer.from(JSON.stringify({ version: 1, id, op, ...fields }));
      assert.ok(body.length <= 4092);
      const frame = Buffer.alloc(body.length + 4);
      frame.writeUInt32BE(body.length);
      body.copy(frame, 4);
      assert.equal(writeSync(command.write, frame), frame.length);
      return (await journal.wait((entry) => entry.kind === "reply" && entry.detail.id === id)).detail;
    },
    wait: async (kind: string): Promise<ConformanceEvent> => (await journal.wait((entry) => entry.kind === "reply" && entry.detail.kind === kind)).detail,
    entries: journal.entries,
    close: (): void => {
      if (closed) return;
      closed = true;
      if (!started) {
        libc.symbols.close(command.read);
        libc.symbols.close(reply.write);
      }
      libc.symbols.close(command.write);
      input.destroy();
    },
  };
};
