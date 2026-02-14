import { resolve } from "node:path";
import { defineConfig, type UserConfig } from "vite";
import react from "@vitejs/plugin-react";

const outDir = resolve(__dirname, "../public/assets");
const ssrEntry = resolve(__dirname, "src/entry-ssr.tsx");
const clientEntry = resolve(__dirname, "src/entry-client.tsx");

export default defineConfig(({ mode }) => {
  const isSsrBundle = mode === "ssr";
  const build: UserConfig["build"] = {
    outDir,
    emptyOutDir: false,
    sourcemap: true,
    minify: false,
    cssCodeSplit: false
  };

  if (isSsrBundle) {
    build.lib = {
      entry: ssrEntry,
      formats: ["iife"],
      name: "HydraSsrBundle",
      fileName: () => "ssr-bundle.js"
    };
    build.rollupOptions = {
      output: {
        assetFileNames: (assetInfo) => {
          if (assetInfo.name && assetInfo.name.endsWith(".css")) {
            return "app.css";
          }
          return "[name][extname]";
        }
      }
    };
  } else {
    build.manifest = "manifest.json";
    build.rollupOptions = {
      input: clientEntry,
      output: {
        format: "iife",
        name: "HydraClientBundle",
        entryFileNames: "client-[hash].js",
        assetFileNames: (assetInfo) => {
          if (assetInfo.name && assetInfo.name.endsWith(".css")) {
            return "app-[hash][extname]";
          }
          return "[name]-[hash][extname]";
        }
      }
    };
  }

  return {
    plugins: [react()],
    define: {
      "process.env.NODE_ENV": JSON.stringify("production")
    },
    build
  };
});
