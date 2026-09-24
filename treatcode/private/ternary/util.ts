import { createHash } from "node:crypto";

export function canonical(value: unknown): string {
  const normalize = (item: unknown): unknown => {
    if (Array.isArray(item)) return item.map(normalize);
    if (item && typeof item === "object") return Object.fromEntries(Object.entries(item).filter(([,v]) => v !== undefined).sort(([a],[b]) => a < b ? -1 : a > b ? 1 : 0).map(([k,v]) => [k, normalize(v)]));
    return item;
  };
  return JSON.stringify(normalize(value));
}
export const hash = (value: unknown) => createHash("sha256").update(typeof value === "string" || value instanceof Uint8Array ? value : canonical(value)).digest("hex");
export class LabError extends Error {
  constructor(readonly code: string, message: string, readonly status = 400) { super(message); }
}
export function invariant(ok: unknown, message: string): asserts ok { if (!ok) throw new LabError("invalid_input", message); }
export function integer(value: unknown, min: number, max: number, name: string): number {
  invariant(typeof value === "number" && Number.isSafeInteger(value) && value >= min && value <= max, `${name} must be an integer between ${min} and ${max}.`);
  return value;
}
export const now = () => new Date().toISOString();
