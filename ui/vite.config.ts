import { resolve } from "node:path";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

const outDir = resolve(__dirname, "../public/assets");

export default defineConfig(({ mode }) => {
  const isSsrBundle = mode === "ssr";

  return {
    plugins: [react()],
    build: {
      outDir,
      emptyOutDir: false,
      sourcemap: true,
      minify: false,
      cssCodeSplit: false,
      lib: {
        entry: isSsrBundle
          ? resolve(__dirname, "src/entry-ssr.tsx")
          : resolve(__dirname, "src/entry-client.tsx"),
        formats: ["iife"],
        name: isSsrBundle ? "HydraSsrBundle" : "HydraClientBundle",
        fileName: () => (isSsrBundle ? "ssr-bundle.js" : "client.js")
      },
      rollupOptions: {
        output: {
          assetFileNames: (assetInfo) => {
            if (assetInfo.name && assetInfo.name.endsWith(".css")) {
              return "app.css";
            }
            return "[name][extname]";
          }
        }
      }
    }
  };
});
