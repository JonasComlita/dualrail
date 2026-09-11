function canonicalFamily(value) {
  return `${value || ""}`.trim().toLowerCase().split(/[/:]/).filter(Boolean).at(-1) || "";
}

export function selectIntelligenceV31CalibrationFamilies(taskId, authorModelFamily, assignments) {
  const match = /^TC-V31-FINAL-(\d{3})$/.exec(taskId || "");
  if (!match) throw new Error(`invalid final task id: ${taskId}`);
  const unique = [];
  for (const assignment of assignments || []) {
    const family = canonicalFamily(assignment?.model_family);
    if (!/^[a-z0-9]+(?:[.-][a-z0-9]+)*$/.test(family) || unique.some((item) => item.model_family === family)) throw new Error(`${taskId}: calibration assignments contain an invalid or duplicate model family`);
    unique.push({ ...assignment, model_family: family });
  }
  const author = canonicalFamily(authorModelFamily);
  const eligible = unique.filter((assignment) => assignment.model_family !== author);
  if (eligible.length < 2) throw new Error(`${taskId}: fewer than two non-author calibration families are available`);
  if (eligible.length === 2) return eligible;
  const offset = (Number(match[1]) - 1) % eligible.length;
  return [eligible[offset], eligible[(offset + 1) % eligible.length]];
}
