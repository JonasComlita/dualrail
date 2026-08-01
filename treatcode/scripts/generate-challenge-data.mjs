import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const manifestPath = path.join(repoRoot, "CHALLENGE_MANIFEST.json");
const generatedRoot = path.join(appRoot, "src", "generated");
const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));

function writeJson(fileName, value) {
  fs.mkdirSync(generatedRoot, { recursive: true });
  const destination = path.join(generatedRoot, fileName);
  fs.writeFileSync(destination, `${JSON.stringify(value, null, 2)}\n`, "utf8");
}

const generatedHeader = {
  schema: "treatcode.challenge_data.v1",
  manifest_schema: manifest.schema,
  manifest_version: manifest.version,
  source_of_truth: manifest.source_of_truth,
  facet_dimensions: manifest.facet_dimensions,
};

const clientChallenges = manifest.challenges
  .filter((challenge) => challenge.lifecycle !== "retired")
  .map((challenge) => ({
    id: challenge.id,
    title: challenge.title,
    lifecycle: challenge.lifecycle,
    difficulty: challenge.difficulty,
    category: challenge.category,
    tags: challenge.tags,
    facets: challenge.facets,
    description: challenge.description,
    signature: challenge.signature,
    template: challenge.template,
    stats: challenge.stats,
    ...challenge.stats,
    execution: {
      mode: challenge.execution.mode,
      runner: challenge.execution.runner,
      limits: challenge.execution.limits,
    },
  }));

writeJson("challenges.client.json", {
  ...generatedHeader,
  challenges: clientChallenges,
});

writeJson("challenges.server.json", {
  ...generatedHeader,
  challenges: manifest.challenges,
});

console.log(`generated challenge data for ${manifest.challenges.length} manifest entries (${clientChallenges.length} client entries)`);
