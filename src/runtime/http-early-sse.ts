type EarlySseHeaders = Readonly<{ headers: Headers }>;
type Entry = { commit: (value: EarlySseHeaders) => boolean; committed: boolean };
const readiness = new WeakMap<Request, Entry>();

/** Physical transport registration only; native metadata never creates this capability. */
export const registerEarlySseHeaders = (request: Request, commit: Entry["commit"]): (() => void) => {
  readiness.set(request, { commit, committed: false });
  return () => readiness.delete(request);
};

/** Synchronous and failure-contained so the provider keeps its original admission receipt. */
export const tryCommitEarlySseHeaders = (request: Request, value: EarlySseHeaders): boolean => {
  const entry = readiness.get(request);
  if (!entry) return false;
  if (entry.committed) return true;
  try {
    entry.committed = entry.commit(value);
  } catch {
    return false;
  }
  return entry.committed;
};

export const earlySseHeadersCommitted = (request: Request): boolean => readiness.get(request)?.committed === true;
