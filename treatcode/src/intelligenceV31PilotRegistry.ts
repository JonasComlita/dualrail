import os from "node:os";
import path from "node:path";
import type { IntelligenceV31CommandRegistry, IntelligenceV31CommandSpec } from "./intelligenceV31Repository";

export class IntelligenceV31PilotCommandRegistry implements IntelligenceV31CommandRegistry {
  constructor(
    private readonly repositoryRoot: string,
    private readonly privateRoot = process.env.TREATCODE_V31_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private", "pilot"),
  ) {}

  resolve(commandId: string, context: { task_id: string; workspace_root: string; visibility: "public" | "private" }): IntelligenceV31CommandSpec | undefined {
    const evaluator = path.join(this.repositoryRoot, "treatcode", "scripts", "intelligence-v31-task-evaluator.mjs");
    const tritc = path.join(this.repositoryRoot, "build", process.platform === "win32" ? "tritc.exe" : "tritc");
    const publicCases = path.join(context.workspace_root, "public", "cases.json");
    const privateNames: Record<string, string> = {
      "pilot.private.behavioral": "behavioral.cases.v3.1.json",
      "pilot.private.adversarial": "adversarial.cases.v3.1.json",
      "pilot.private.performance": "performance.cases.v3.1.json",
    };
    if (commandId === "pilot.public" && context.visibility === "public") return { executable: process.execPath, args: [evaluator, `--workspace=${context.workspace_root}`, `--cases=${publicCases}`, `--tritc=${tritc}`, "--visibility=public"] };
    const privateName = privateNames[commandId];
    if (privateName && context.visibility === "private") return { executable: process.execPath, args: [evaluator, `--workspace=${context.workspace_root}`, `--cases=${path.join(this.privateRoot, context.task_id, privateName)}`, `--tritc=${tritc}`, "--visibility=private"] };
    return undefined;
  }
}
