import { readFileSync } from "node:fs";
import path from "node:path";
import {
  scoreIntelligenceV3Run,
  validateIntelligenceV3Protocol,
  type IntelligenceV3Protocol,
  type IntelligenceV3RunScore,
  type IntelligenceV3TaskResult,
} from "./intelligenceV3";
import type { IntelligenceV3DiscussionRubric } from "./intelligenceV3Discussion";

export interface IntelligenceV3CandidateTask {
  id: string;
  category: string;
  title: string;
  symptom: string;
  state: "needs_independent_author" | "authoring" | "review" | "calibration" | "frozen";
  provenance: { author_id: string | null; reviewer_ids: string[]; subject_model_independence_verified: boolean };
  packaging: { participant_bundle_hash: string | null; grader_bundle_hash: string | null; isolation_audit_hash: string | null };
  calibration: { status: string; cohort_pass_rates: Record<string, number> };
  holdout: { frozen: boolean; manifest_hash: string | null };
}

export interface IntelligenceV3CandidateCorpus {
  schema: "treatcode.intelligence.corpus-candidates.v3";
  version: 3;
  status: "authoring" | "review" | "calibration" | "frozen";
  official: boolean;
  task_count: number;
  warning: string;
  category_counts: Record<string, number>;
  tasks: IntelligenceV3CandidateTask[];
}

export interface IntelligenceV3CatalogServiceOptions {
  protocolPath?: string;
  corpusPath?: string;
  draftsPath?: string;
  discussionRubricPath?: string;
}

interface IntelligenceV3ExecutableDrafts {
  schema: "treatcode.intelligence.executable-drafts.v3";
  version: 3;
  official_eligible: false;
  tasks: Array<{ task_id: string; grader_outside_subject_workspace: boolean; one_shot_hidden_submission: boolean; starter: { public_passed: boolean; hidden_passed: boolean }; private_known_good: { public_passed: boolean; hidden_passed: boolean } }>;
}

function defaultV3Root(): string {
  return path.resolve(__dirname, "..", "..", "benchmarks", "intelligence-v3");
}

function readJson<T>(filePath: string): T {
  return JSON.parse(readFileSync(filePath, "utf8")) as T;
}

export class IntelligenceV3CatalogService {
  readonly protocol: IntelligenceV3Protocol;
  readonly corpus: IntelligenceV3CandidateCorpus;
  readonly drafts: IntelligenceV3ExecutableDrafts;
  readonly discussionRubric: IntelligenceV3DiscussionRubric;
  readonly protocolIssues: string[];

  constructor(options: IntelligenceV3CatalogServiceOptions = {}) {
    const root = defaultV3Root();
    this.protocol = readJson<IntelligenceV3Protocol>(options.protocolPath || path.join(root, "protocol.v3.json"));
    this.corpus = readJson<IntelligenceV3CandidateCorpus>(options.corpusPath || path.join(root, "corpus.candidates.v3.json"));
    this.drafts = readJson<IntelligenceV3ExecutableDrafts>(options.draftsPath || path.join(root, "executable-drafts.v3.json"));
    this.discussionRubric = readJson<IntelligenceV3DiscussionRubric>(options.discussionRubricPath || path.join(root, "discussion-rubric.v3.json"));
    this.protocolIssues = validateIntelligenceV3Protocol(this.protocol);
    if (this.protocolIssues.length > 0) throw new Error(`Invalid intelligence v3 protocol: ${this.protocolIssues.join("; ")}`);
    if (this.corpus.schema !== "treatcode.intelligence.corpus-candidates.v3" || this.corpus.version !== 3 || this.corpus.task_count !== this.corpus.tasks.length) throw new Error("Invalid intelligence v3 candidate corpus");
    const ids = new Set(this.corpus.tasks.map((task) => task.id));
    if (ids.size !== this.corpus.tasks.length) throw new Error("Intelligence v3 candidate corpus contains duplicate task ids");
    if (this.corpus.task_count < this.protocol.official_requirements.minimum_distinct_tasks) throw new Error("Intelligence v3 candidate corpus is smaller than the protocol minimum");
    if (this.drafts.schema !== "treatcode.intelligence.executable-drafts.v3" || this.drafts.version !== 3 || this.drafts.official_eligible !== false || new Set(this.drafts.tasks.map((task) => task.task_id)).size !== this.drafts.tasks.length || this.drafts.tasks.some((task) => !ids.has(task.task_id))) throw new Error("Invalid intelligence v3 executable draft registry");
    if (this.discussionRubric.schema !== "treatcode.intelligence.discussion-rubric.v3" || this.discussionRubric.version !== 3 || this.discussionRubric.included_in_executable_score !== false || this.discussionRubric.length.maximum_words !== this.protocol.dimensions.discussion.maximum_words || this.discussionRubric.rating.minimum_raters < 2) throw new Error("Invalid intelligence v3 discussion rubric");
  }

  catalog() {
    const authored = this.corpus.tasks.filter((task) => task.provenance.author_id).length;
    const reviewed = this.corpus.tasks.filter((task) => new Set(task.provenance.reviewer_ids).size >= this.protocol.official_requirements.minimum_reviewers_per_task).length;
    const packaged = this.corpus.tasks.filter((task) => task.packaging.participant_bundle_hash && task.packaging.grader_bundle_hash && task.packaging.isolation_audit_hash).length;
    const calibrated = this.corpus.tasks.filter((task) => task.calibration.status === "accepted").length;
    const frozen = this.corpus.tasks.filter((task) => task.holdout.frozen).length;
    return {
      schema: "treatcode.intelligence.catalog.v3" as const,
      official: this.corpus.official && this.protocol.status === "frozen" && this.protocol.calibration.status === "complete",
      score_scope: this.protocol.score_scope,
      protocol: this.protocol,
      corpus: {
        schema: this.corpus.schema,
        version: this.corpus.version,
        status: this.corpus.status,
        official: this.corpus.official,
        warning: this.corpus.warning,
        task_count: this.corpus.task_count,
        category_counts: { ...this.corpus.category_counts },
        readiness: { executable_drafts: this.drafts.tasks.length, authored, reviewed, packaged, calibrated, frozen, discussion_rubric_calibrated: this.discussionRubric.status === "calibrated" || this.discussionRubric.status === "frozen", required: this.protocol.official_requirements.minimum_distinct_tasks },
        tasks: this.corpus.tasks.map((task) => ({ id: task.id, category: task.category, title: task.title, symptom: task.symptom, state: task.state })),
      },
    };
  }

  scoreRun(results: IntelligenceV3TaskResult[], seed?: number): IntelligenceV3RunScore {
    return scoreIntelligenceV3Run(this.protocol, results, this.corpus.tasks.map((task) => task.id), seed);
  }
}

export const intelligenceV3CatalogService = new IntelligenceV3CatalogService();
