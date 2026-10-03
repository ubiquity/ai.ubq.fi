import assert from "node:assert/strict";
import { toast } from "../static/toast.js";

class ToastElement {
  readonly dataset: Record<string, string> = {};
  readonly children: ToastElement[] = [];
  readonly listeners = new Map<string, () => void>();
  textContent = "";
  parent: ToastElement | null = null;
  private _queries = 0;

  constructor(private readonly _root = false) {}

  get isConnected(): boolean {
    return this.parent?.isConnected ?? this._root;
  }

  setAttribute(name: string, value: string): void {
    if (name.startsWith("data-")) this.dataset[name.slice(5)] = value;
  }

  appendChild(child: ToastElement): void {
    child.parent = this;
    this.children.push(child);
  }

  append(child: ToastElement): void {
    this.appendChild(child);
  }

  insertBefore(child: ToastElement, reference: ToastElement): void {
    child.parent = this;
    const index = this.children.indexOf(reference);
    if (index === -1) this.children.push(child);
    else this.children.splice(index, 0, child);
  }

  addEventListener(name: string, callback: () => void): void {
    this.listeners.set(name, callback);
  }

  querySelectorAll(selector: string): ToastElement[] {
    if (++this._queries > 100) throw new Error("Toast eviction did not converge");
    if (selector === "[data-toast-desc]") {
      return this.children.filter((child) => "toast-desc" in child.dataset);
    }
    assert.ok(selector === "[data-toast]" || selector === "[data-toast]:not([data-exiting])");
    return this.children.filter((child) => "toast" in child.dataset && (selector === "[data-toast]" || !("exiting" in child.dataset)));
  }

  querySelector(selector: string): ToastElement | null {
    return this.querySelectorAll(selector)[0] ?? null;
  }

  remove(): void {
    if (this.parent) this.parent.children.splice(this.parent.children.indexOf(this), 1);
    this.parent = null;
  }
}

Deno.test("a synchronous toast burst keeps three active notifications while older nodes exit", () => {
  const body = new ToastElement(true);
  const timers: { callback: () => void; delay: number }[] = [];
  const replacements = {
    document: {
      body,
      createElement: () => new ToastElement(),
      createElementNS: () => new ToastElement(),
    },
    requestAnimationFrame: (callback: () => void) => {
      callback();
    },
    setTimeout: (callback: () => void, delay: number) => {
      timers.push({ callback, delay });
      return timers.length;
    },
  };

  const originals = new Map(Object.keys(replacements).map((name) => [name, Object.getOwnPropertyDescriptor(globalThis, name)]));
  try {
    for (const [name, value] of Object.entries(replacements)) Object.defineProperty(globalThis, name, { configurable: true, writable: true, value });
    // The query budget makes a non-converging synchronous loop fail instead of hanging the test runner.
    for (let i = 1; i <= 6; i++) toast.info(String(i), { duration: 0 });

    const host = body.children[0];
    assert.equal(host.children.length, 6, "evicted nodes remain during the exit animation");
    assert.deepEqual(
      host.querySelectorAll("[data-toast]:not([data-exiting])").map((element) => element.children[0].textContent),
      ["4", "5", "6"],
      "the oldest active notifications are evicted first"
    );
    assert.deepEqual(
      timers.map((timer) => timer.delay),
      [160, 160, 160]
    );
    for (const timer of timers) timer.callback();
    assert.equal(host.children.length, 3, "exit timers remove the evicted nodes");
    assert.equal(host.querySelectorAll("[data-toast]:not([data-exiting])").length, 3);
  } finally {
    if (body.children[0]) body.children[0].remove();
    for (const [name, original] of originals) {
      if (original) Object.defineProperty(globalThis, name, original);
      else Reflect.deleteProperty(globalThis, name);
    }
  }
});

Deno.test("loading toasts with Infinity duration do not auto-dismiss after 4000 ms", () => {
  const body = new ToastElement(true);
  let now = 0;
  const timers: { callback: () => void; delay: number; fireAt: number; cleared?: boolean }[] = [];
  const replacements = {
    document: {
      body,
      createElement: () => new ToastElement(),
      createElementNS: () => new ToastElement(),
    },
    requestAnimationFrame: (callback: () => void) => {
      callback();
    },
    setTimeout: (callback: () => void, delay: number) => {
      const timer = { callback, delay, fireAt: now + delay };
      timers.push(timer);
      return timers.length;
    },
    clearTimeout: (id: number) => {
      if (id > 0 && id <= timers.length) {
        timers[id - 1].cleared = true;
      }
    },
  };

  const originals = new Map(Object.keys(replacements).map((name) => [name, Object.getOwnPropertyDescriptor(globalThis, name)]));
  try {
    for (const [name, value] of Object.entries(replacements)) {
      Object.defineProperty(globalThis, name, { configurable: true, writable: true, value });
    }

    const handle = toast.loading("Processing operation...");
    const host = body.children[0];
    assert.equal(host.children.length, 1);
    const toastEl = host.children[0];
    assert.equal(toastEl.dataset.type, "info");
    assert.equal(toastEl.children[0].textContent, "Processing operation...");

    // Verify no 4000 ms auto-dismiss timer is scheduled
    const autoDismissTimers = timers.filter((t) => t.delay === 4000);
    assert.equal(autoDismissTimers.length, 0, "no auto-dismiss timer is scheduled for loading toast");

    // Advance clock past 4000 ms
    now += 4000;
    for (const timer of timers) {
      if (!timer.cleared && timer.fireAt <= now) {
        timer.callback();
      }
    }

    // Node is still present and active
    assert.equal(host.children.length, 1);
    assert.equal("exiting" in toastEl.dataset, false, "loading toast remains present past 4000 ms");

    // Update title and description; node remains present
    handle.update({ title: "Operation completed", description: "All items processed successfully." });
    assert.equal(toastEl.children[0].textContent, "Operation completed");
    assert.equal("exiting" in toastEl.dataset, false, "toast remains present after update");

    // Dismiss manually
    handle.dismiss();
    assert.equal("exiting" in toastEl.dataset, true, "toast enters exiting state on dismiss");

    // Advance through exit animation (160 ms)
    now += 160;
    for (const timer of timers) {
      if (!timer.cleared && timer.fireAt <= now) {
        timer.callback();
      }
    }
    assert.equal(host.children.length, 0, "toast node is removed after dismiss animation");
  } finally {
    if (body.children[0]) body.children[0].remove();
    for (const [name, original] of originals) {
      if (original) Object.defineProperty(globalThis, name, original);
      else Reflect.deleteProperty(globalThis, name);
    }
  }
});
