// Run the focused account/community checks in
// one script so direct and package-manager invocations exercise identical code.
import "./test-account-community";

import { mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";

const evidenceRoot = path.resolve(import.meta.dir, "..", "..", "build", "treatcode-community-tests");
mkdirSync(evidenceRoot, { recursive: true });
writeFileSync(path.join(evidenceRoot, "community-tests.json"), `${JSON.stringify({
  schema: "trit.treatcode_community_tests.v1",
  ok: true,
  checks: ["durable participant account", "restart login", "versioned solution", "public posted solution discussion feed", "accepted-source solved marker", "measured runtime and memory metrics", "durable solution votes", "authenticated submission", "plain-text discussion", "metadata secret scrubbing"],
}, null, 2)}\n`);
