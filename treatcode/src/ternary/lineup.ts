import type { ModelConfig } from "./contracts";

/** User-selected pilot cohort. Preselection is fixed before observing results. */
export const HARNESS_LINEUP: ModelConfig[] = ["gpt-6-astra","gpt-5.6-sol","gpt-5.6-terra","gpt-5.6-luna"].map(model=>({
  id:`${model}-high`,provider:"codex",model,keyEnv:"CODEX_LOCAL_SESSION",
  settings:{"reasoning.effort":"high"},supportedSettings:{"reasoning.effort":["high"]},maxOutputTokens:24000,pricing:null,
}));
export const HARNESS_TIERS:Record<string,"lower"|"middle"|"higher">={
  "gpt-6-astra-high":"higher","gpt-5.6-sol-high":"higher","gpt-5.6-terra-high":"middle","gpt-5.6-luna-high":"lower",
};
