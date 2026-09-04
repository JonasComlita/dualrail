import fs from "node:fs";
import path from "node:path";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

function learningRouteInputs(): Record<string, string> {
  const root = path.resolve(__dirname, "learn");
  if (!fs.existsSync(root)) return {};
  const inputs: Record<string, string> = {};
  const visit = (directory: string) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const entryPath = path.join(directory, entry.name);
      if (entry.isDirectory()) visit(entryPath);
      else if (entry.name === "index.html" && entryPath !== path.join(root, "index.html")) {
        const relative = path.relative(root, entryPath).split(path.sep).join("/");
        inputs[`learn-${relative.replace(/\/index\.html$/, "").replace(/\//g, "-")}`] = entryPath;
      }
    }
  };
  visit(root);
  return inputs;
}

const learningInputs = learningRouteInputs();
const apiPort = process.env.TREATCODE_API_PORT || "3000";
const vitePort = Number.parseInt(process.env.TREATCODE_VITE_PORT || "5173", 10);

export default defineConfig({
  plugins: [react()],
  server: {
    port: vitePort,
    proxy: {
      "/api": {
        target: `http://127.0.0.1:${apiPort}`,
        changeOrigin: true,
        secure: false,
      },
    },
  },
  build: {
    outDir: "dist",
    emptyOutDir: true,
    rollupOptions: {
      input: {
        overview: "index.html",
        stack: "stack/index.html",
        learn: "learn/index.html",
        practice: "practice/index.html",
        operations: "operations/index.html",
        arena: "arena/index.html",
        intelligence: "intelligence/index.html",
        ...learningInputs,
      },
    },
  },
});
