import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/api": {
        target: "http://localhost:3000",
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
        operations: "operations/index.html",
        arena: "arena/index.html",
      },
    },
  },
});
