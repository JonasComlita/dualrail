import os from "node:os";
import path from "node:path";
import type { IntelligenceV31CommandRegistry, IntelligenceV31CommandSpec } from "./intelligenceV31Repository";

export class IntelligenceV31FinalCommandRegistry implements IntelligenceV31CommandRegistry {
  constructor(
    private readonly repositoryRoot: string,
    private readonly privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private"),
  ) {}

  resolve(commandId: string, context: { task_id: string; workspace_root: string; visibility: "public" | "private" }): IntelligenceV31CommandSpec | undefined {
    if (!/^TC-V31-FINAL-\d{3}$/.test(context.task_id)) return undefined;
    const suite = commandId === "final.public" && context.visibility === "public"
      ? "public"
      : commandId === "final.private.behavioral" && context.visibility === "private"
        ? "behavioral"
        : commandId === "final.private.adversarial" && context.visibility === "private"
          ? "adversarial"
          : commandId === "final.private.performance" && context.visibility === "private"
            ? "performance"
            : null;
    if (!suite) return undefined;
    const evaluator = path.join(this.repositoryRoot, "treatcode", "scripts", "intelligence-v31-final-task-evaluator.mjs");
    return {
      executable: process.execPath,
      args: [
        evaluator,
        `--workspace=${context.workspace_root}`,
        `--private-root=${this.privateRoot}`,
        `--task=${context.task_id}`,
        `--suite=${suite}`,
      ],
    };
  }
}
