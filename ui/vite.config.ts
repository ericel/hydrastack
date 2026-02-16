import fs from "node:fs";
import { resolve } from "node:path";
import postcss from "postcss";
import { defineConfig, type Plugin, type UserConfig } from "vite";
import react from "@vitejs/plugin-react";

const outDir = resolve(__dirname, "../public/assets");
const ssrEntry = resolve(__dirname, "src/entry-ssr.tsx");
const clientEntry = resolve(__dirname, "src/entry-client.tsx");
const sourceDir = resolve(__dirname, "src");

type CssObfuscationConfig = {
  enabled: boolean;
  emitClassmap: boolean;
};

type HydraPluginConfig = Record<string, unknown>;
const GENERATED_ASSET_PATTERN = /^.+-[a-z0-9_-]{8,}\.[a-z0-9]+(?:\.map)?$/i;
const LEGACY_GENERATED_ASSET_FILES = new Set([
  "client.js",
  "client.js.map",
  "app.js",
  "app.js.map"
]);
const PRESERVED_ASSET_FILES = new Set([
  "ssr-bundle.js",
  "ssr-bundle.js.map",
  "manifest.json",
  "classmap.json",
  "app.css",
  "app.css.map"
]);

function readJsonFile(filePath: string): Record<string, unknown> | null {
  if (!fs.existsSync(filePath)) {
    return null;
  }
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf8")) as Record<string, unknown>;
  } catch {
    return null;
  }
}

function resolveHydraConfigPath(): string {
  const envPath = process.env.HYDRA_UI_CONFIG_PATH ?? process.env.HYDRA_CONFIG_PATH;
  if (envPath) {
    return resolve(__dirname, "..", envPath);
  }
  const demoConfig = resolve(__dirname, "../demo/config.json");
  if (fs.existsSync(demoConfig)) {
    return demoConfig;
  }
  return resolve(__dirname, "../app/config.json");
}

function readHydraPluginConfig(): HydraPluginConfig {
  const configPath = resolveHydraConfigPath();
  const parsed = readJsonFile(configPath);
  if (!parsed) {
    return {};
  }

  const plugins = Array.isArray(parsed.plugins) ? parsed.plugins : [];
  for (const plugin of plugins) {
    if (
      plugin &&
      typeof plugin === "object" &&
      (plugin as Record<string, unknown>).name === "hydra::HydraSsrPlugin" &&
      (plugin as Record<string, unknown>).config &&
      typeof (plugin as Record<string, unknown>).config === "object"
    ) {
      return (plugin as Record<string, unknown>).config as HydraPluginConfig;
    }
  }

  return {};
}

function readCssObfuscationConfig(): CssObfuscationConfig {
  const pluginConfig = readHydraPluginConfig();
  const raw =
    pluginConfig.css_obfuscation ?? pluginConfig.cssObfuscation ?? pluginConfig.obfuscation;

  let enabled = false;
  let emitClassmap = true;

  if (typeof raw === "boolean") {
    enabled = raw;
  } else if (raw && typeof raw === "object") {
    const objectValue = raw as Record<string, unknown>;
    enabled = Boolean(objectValue.enabled);

    const emitSnake = objectValue.emit_classmap;
    const emitCamel = objectValue.emitClassmap;
    if (typeof emitSnake === "boolean") {
      emitClassmap = emitSnake;
    } else if (typeof emitCamel === "boolean") {
      emitClassmap = emitCamel;
    }
  }

  if (process.env.HYDRA_CSS_OBFUSCATION === "1") {
    enabled = true;
  }
  if (process.env.HYDRA_CSS_OBFUSCATION === "0") {
    enabled = false;
  }

  return { enabled, emitClassmap };
}

function collectSourceFiles(dirPath: string): string[] {
  const out: string[] = [];
  if (!fs.existsSync(dirPath)) {
    return out;
  }

  for (const entry of fs.readdirSync(dirPath, { withFileTypes: true })) {
    const fullPath = resolve(dirPath, entry.name);
    if (entry.isDirectory()) {
      out.push(...collectSourceFiles(fullPath));
      continue;
    }

    if (!entry.isFile()) {
      continue;
    }

    if (/\.(tsx?|jsx?)$/.test(entry.name)) {
      out.push(fullPath);
    }
  }

  return out;
}

function extractClassLiterals(code: string): string[] {
  const literals: string[] = [];
  const patterns = [
    /className\s*=\s*"([^"]*)"/g,
    /className\s*=\s*'([^']*)'/g,
    /className\s*=\s*\{\s*"([^"]*)"\s*\}/g,
    /className\s*=\s*\{\s*'([^']*)'\s*\}/g,
    /className\s*=\s*\{\s*`([^`$]*)`\s*\}/g
  ];

  for (const pattern of patterns) {
    for (const match of code.matchAll(pattern)) {
      const value = match[1];
      if (value) {
        literals.push(value);
      }
    }
  }

  return literals;
}

function splitClassTokens(value: string): string[] {
  return value
    .split(/\s+/)
    .map((token) => token.trim())
    .filter((token) => token.length > 0);
}

function fnv1a32(input: string): number {
  let hash = 0x811c9dc5;
  for (let index = 0; index < input.length; index += 1) {
    hash ^= input.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193);
  }
  return hash >>> 0;
}

function buildClassMap(): Map<string, string> {
  const tokens = new Set<string>();
  for (const filePath of collectSourceFiles(sourceDir)) {
    const source = fs.readFileSync(filePath, "utf8");
    for (const literal of extractClassLiterals(source)) {
      for (const token of splitClassTokens(literal)) {
        tokens.add(token);
      }
    }
  }

  const sorted = Array.from(tokens).sort();
  const used = new Set<string>();
  const classMap = new Map<string, string>();

  for (const token of sorted) {
    const hashSeed = fnv1a32(token).toString(36);
    let candidate = `h${hashSeed}`;
    let suffix = 0;
    while (used.has(candidate)) {
      suffix += 1;
      candidate = `h${hashSeed}${suffix.toString(36)}`;
    }
    used.add(candidate);
    classMap.set(token, candidate);
  }

  return classMap;
}

function rewriteClassTokens(value: string, classMap: Map<string, string>): string {
  return value.replace(/\S+/g, (token) => classMap.get(token) ?? token);
}

function rewriteSourceClasses(code: string, classMap: Map<string, string>): string {
  const replaceClassLiteral =
    (pattern: RegExp, replaceTemplate: (value: string) => string): void => {
      code = code.replace(pattern, (_full, literal) => replaceTemplate(literal));
    };

  replaceClassLiteral(
    /className\s*=\s*"([^"]*)"/g,
    (literal) => `className="${rewriteClassTokens(literal, classMap)}"`
  );
  replaceClassLiteral(
    /className\s*=\s*'([^']*)'/g,
    (literal) => `className='${rewriteClassTokens(literal, classMap)}'`
  );
  replaceClassLiteral(
    /className\s*=\s*\{\s*"([^"]*)"\s*\}/g,
    (literal) => `className={"${rewriteClassTokens(literal, classMap)}"}`
  );
  replaceClassLiteral(
    /className\s*=\s*\{\s*'([^']*)'\s*\}/g,
    (literal) => `className={'${rewriteClassTokens(literal, classMap)}'}`
  );
  replaceClassLiteral(
    /className\s*=\s*\{\s*`([^`$]*)`\s*\}/g,
    (literal) => `className={\`${rewriteClassTokens(literal, classMap)}\`}`
  );
  replaceClassLiteral(
    /className:\s*"([^"]*)"/g,
    (literal) => `className: "${rewriteClassTokens(literal, classMap)}"`
  );
  replaceClassLiteral(
    /className:\s*'([^']*)'/g,
    (literal) => `className: '${rewriteClassTokens(literal, classMap)}'`
  );

  return code;
}

function escapeRegex(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function escapeCssClassToken(value: string): string {
  // Tailwind-like selector escaping (sufficient for generated utility classes).
  return value.replace(/([^a-zA-Z0-9_-])/g, "\\$1");
}

function rewriteSelectors(selector: string, classMap: Map<string, string>): string {
  let rewritten = selector;
  for (const [token, mangled] of classMap.entries()) {
    const escapedToken = escapeCssClassToken(token);
    const pattern = new RegExp(`\\.${escapeRegex(escapedToken)}(?![a-zA-Z0-9_-])`, "g");
    rewritten = rewritten.replace(pattern, `.${mangled}`);
  }
  return rewritten;
}

function rewriteCssClasses(css: string, classMap: Map<string, string>): string {
  const root = postcss.parse(css);
  root.walkRules((rule) => {
    rule.selector = rewriteSelectors(rule.selector, classMap);
  });
  return root.toString();
}

function createCssObfuscationPlugin(
  options: CssObfuscationConfig & { isSsrBundle: boolean }
): Plugin {
  const classMap = options.enabled ? buildClassMap() : new Map<string, string>();

  return {
    name: "hydra-css-obfuscation",
    apply: "build",
    enforce: "pre",
    transform(code, id) {
      if (!options.enabled) {
        return null;
      }
      if (id.includes("node_modules")) {
        return null;
      }
      if (!/\.(tsx?|jsx?)$/.test(id)) {
        return null;
      }
      if (!id.includes("/src/")) {
        return null;
      }

      const transformed = rewriteSourceClasses(code, classMap);
      if (transformed === code) {
        return null;
      }
      return {
        code: transformed,
        map: null
      };
    },
    generateBundle(_outputOptions, bundle) {
      if (!options.enabled) {
        return;
      }

      if (!options.isSsrBundle && options.emitClassmap) {
        const classmapObject = Object.fromEntries(classMap.entries());
        this.emitFile({
          type: "asset",
          fileName: "classmap.json",
          source: JSON.stringify(classmapObject, null, 2)
        });
      }
    },
    writeBundle(outputOptions, bundle) {
      if (!options.enabled) {
        return;
      }

      const outputDir =
        typeof outputOptions.dir === "string" ? outputOptions.dir : outDir;
      for (const emitted of Object.values(bundle)) {
        if (emitted.type !== "asset" || !emitted.fileName.endsWith(".css")) {
          continue;
        }

        const cssPath = resolve(outputDir, emitted.fileName);
        if (!fs.existsSync(cssPath)) {
          continue;
        }

        const cssSource = fs.readFileSync(cssPath, "utf8");
        const rewritten = rewriteCssClasses(cssSource, classMap);
        fs.writeFileSync(cssPath, rewritten, "utf8");
      }
    }
  };
}

function createPruneHashedAssetsPlugin(options: { enabled: boolean }): Plugin {
  return {
    name: "hydra-prune-hashed-assets",
    apply: "build",
    writeBundle(outputOptions, bundle) {
      if (!options.enabled) {
        return;
      }

      const outputDir = typeof outputOptions.dir === "string" ? outputOptions.dir : outDir;
      if (!fs.existsSync(outputDir)) {
        return;
      }

      const emittedFiles = new Set<string>();
      for (const emitted of Object.values(bundle)) {
        emittedFiles.add(emitted.fileName);
      }

      for (const entry of fs.readdirSync(outputDir, { withFileTypes: true })) {
        if (!entry.isFile()) {
          continue;
        }

        const fileName = entry.name;
        if (PRESERVED_ASSET_FILES.has(fileName) || emittedFiles.has(fileName)) {
          continue;
        }
        if (LEGACY_GENERATED_ASSET_FILES.has(fileName)) {
          fs.unlinkSync(resolve(outputDir, fileName));
          continue;
        }
        if (!GENERATED_ASSET_PATTERN.test(fileName)) {
          continue;
        }

        fs.unlinkSync(resolve(outputDir, fileName));
      }
    }
  };
}

export default defineConfig(({ mode }) => {
  const isSsrBundle = mode === "ssr";
  const isWatchMode = process.argv.includes("--watch");
  const cssObfuscation = readCssObfuscationConfig();
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
    plugins: [
      createCssObfuscationPlugin({
        enabled: cssObfuscation.enabled,
        emitClassmap: cssObfuscation.emitClassmap,
        isSsrBundle
      }),
      createPruneHashedAssetsPlugin({
        enabled: !isSsrBundle && !isWatchMode
      }),
      react()
    ],
    server: {
      host: "127.0.0.1",
      port: 5174,
      strictPort: true
    },
    define: {
      "process.env.NODE_ENV": JSON.stringify("production")
    },
    build
  };
});
