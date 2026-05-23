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
  const appConfig = resolve(__dirname, "../app/config.json");
  if (fs.existsSync(appConfig)) {
    return appConfig;
  }
  return resolve(__dirname, "../demo/config.json");
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

function looksLikeClassToken(token: string): boolean {
  if (!token || token.length > 80) return false;

  const bareUtilityTokens = new Set([
    "absolute",
    "block",
    "border",
    "container",
    "contents",
    "fixed",
    "flex",
    "grid",
    "hidden",
    "inline",
    "isolate",
    "relative",
    "static",
    "sticky",
    "sr-only",
    "table",
    "truncate",
  ]);

  if (bareUtilityTokens.has(token)) {
    return true;
  }

  if (!/^[a-z\[\-]/.test(token)) return false;
  if (!/^[a-z0-9\-:_/\[\]().,%@!&]+$/.test(token)) return false;
  return /[-:[\]/()]/.test(token);
}

function looksLikeClassList(literal: string): boolean {
  const trimmed = literal.trim();
  if (!trimmed) return false;
  const tokens = trimmed.split(/\s+/);
  if (tokens.length < 2) return false;
  for (const token of tokens) {
    if (!looksLikeClassToken(token)) return false;
  }
  return true;
}

function isLikelyModuleSpecifierString(source: string, quoteIndex: number): boolean {
  const prefix = source.slice(Math.max(0, quoteIndex - 80), quoteIndex).trimEnd();
  return (
    /\bfrom\s*$/.test(prefix) ||
    /(?:^|[;\n])\s*import\s*$/.test(prefix) ||
    /\bimport\s*\(\s*$/.test(prefix)
  );
}

function findMatchingBrace(source: string, openIndex: number): number {
  let depth = 1;
  let i = openIndex;
  while (i < source.length) {
    const c = source[i];
    if (c === "\\") {
      i += 2;
      continue;
    }
    if (c === '"' || c === "'") {
      const quote = c;
      i++;
      while (i < source.length) {
        if (source[i] === "\\") {
          i += 2;
          continue;
        }
        if (source[i] === quote) {
          i++;
          break;
        }
        i++;
      }
      continue;
    }
    if (c === "`") {
      i++;
      while (i < source.length && source[i] !== "`") {
        if (source[i] === "\\") {
          i += 2;
          continue;
        }
        i++;
      }
      i++;
      continue;
    }
    if (c === "{") {
      depth++;
    } else if (c === "}") {
      depth--;
      if (depth === 0) return i;
    }
    i++;
  }
  return -1;
}

function findMatchingSquareBracket(source: string, openIndex: number): number {
  let depth = 1;
  let i = openIndex + 1;
  while (i < source.length) {
    const c = source[i];
    if (c === "\\") {
      i += 2;
      continue;
    }
    if (c === '"' || c === "'") {
      const quote = c;
      i++;
      while (i < source.length) {
        if (source[i] === "\\") {
          i += 2;
          continue;
        }
        if (source[i] === quote) {
          i++;
          break;
        }
        i++;
      }
      continue;
    }
    if (c === "`") {
      i++;
      while (i < source.length) {
        if (source[i] === "\\") {
          i += 2;
          continue;
        }
        if (source[i] === "$" && source[i + 1] === "{") {
          const closeIdx = findMatchingBrace(source, i + 2);
          if (closeIdx === -1) return -1;
          i = closeIdx + 1;
          continue;
        }
        if (source[i] === "`") {
          i++;
          break;
        }
        i++;
      }
      continue;
    }
    if (c === "[") {
      depth++;
    } else if (c === "]") {
      depth--;
      if (depth === 0) return i;
    }
    i++;
  }
  return -1;
}

function isSpaceJoinSuffix(source: string, closeBracketIndex: number): boolean {
  return /^\s*\.join\(\s*(["'])\s+\1\s*\)/.test(source.slice(closeBracketIndex + 1));
}

function extractFromExpressionBody(body: string, out: string[]): void {
  let i = 0;
  while (i < body.length) {
    const c = body[i];
    if (c === "\\") {
      i += 2;
      continue;
    }
    if (c === '"' || c === "'") {
      const quote = c;
      let j = i + 1;
      while (j < body.length) {
        if (body[j] === "\\") {
          j += 2;
          continue;
        }
        if (body[j] === quote) break;
        j++;
      }
      out.push(body.slice(i + 1, j));
      i = j + 1;
      continue;
    }
    if (c === "`") {
      let j = i + 1;
      while (j < body.length) {
        if (body[j] === "\\") {
          j += 2;
          continue;
        }
        if (body[j] === "`") break;
        if (body[j] === "$" && body[j + 1] === "{") {
          const closeIdx = findMatchingBrace(body, j + 2);
          if (closeIdx === -1) {
            j = body.length;
            break;
          }
          extractFromExpressionBody(body.slice(j + 2, closeIdx), out);
          j = closeIdx + 1;
          continue;
        }
        j++;
      }
      const tmpl = body.slice(i + 1, j);
      let k = 0;
      while (k < tmpl.length) {
        const interp = tmpl.indexOf("${", k);
        if (interp === -1) {
          out.push(tmpl.slice(k));
          break;
        }
        out.push(tmpl.slice(k, interp));
        const close = findMatchingBrace(tmpl, interp + 2);
        if (close === -1) break;
        k = close + 1;
      }
      i = j + 1;
      continue;
    }
    i++;
  }
}

function extractJoinSpaceArrayLiterals(code: string, out: string[]): void {
  for (let i = 0; i < code.length; i++) {
    if (code[i] !== "[") continue;
    const closeIndex = findMatchingSquareBracket(code, i);
    if (closeIndex === -1) continue;
    if (isSpaceJoinSuffix(code, closeIndex)) {
      extractFromExpressionBody(code.slice(i + 1, closeIndex), out);
    }
    i = closeIndex;
  }
}

function extractClassLiterals(code: string): string[] {
  const literals: string[] = [];

  for (const match of code.matchAll(/className\s*=\s*"([^"]*)"/g)) {
    if (match[1]) literals.push(match[1]);
  }
  for (const match of code.matchAll(/className\s*=\s*'([^']*)'/g)) {
    if (match[1]) literals.push(match[1]);
  }
  for (const match of code.matchAll(/className:\s*"([^"]*)"/g)) {
    if (match[1]) literals.push(match[1]);
  }
  for (const match of code.matchAll(/className:\s*'([^']*)'/g)) {
    if (match[1]) literals.push(match[1]);
  }

  const opener = /className\s*=\s*\{/g;
  let match: RegExpExecArray | null;
  while ((match = opener.exec(code)) !== null) {
    const bodyStart = match.index + match[0].length;
    const bodyEnd = findMatchingBrace(code, bodyStart);
    if (bodyEnd === -1) break;
    extractFromExpressionBody(code.slice(bodyStart, bodyEnd), literals);
    opener.lastIndex = bodyEnd + 1;
  }
  extractJoinSpaceArrayLiterals(code, literals);

  for (const match2 of code.matchAll(/"((?:\\.|[^"\\])*)"/g)) {
    if (isLikelyModuleSpecifierString(code, match2.index)) continue;
    const literal = match2[1];
    if (literal && looksLikeClassList(literal.trim())) {
      literals.push(literal);
    }
  }
  for (const match2 of code.matchAll(/'((?:\\.|[^'\\])*)'/g)) {
    if (isLikelyModuleSpecifierString(code, match2.index)) continue;
    const literal = match2[1];
    if (literal && looksLikeClassList(literal.trim())) {
      literals.push(literal);
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

function shouldRewriteAsClassList(literal: string, classMap: Map<string, string>): boolean {
  const trimmed = literal.trim();
  if (!trimmed) return false;

  const tokens = trimmed.split(/\s+/);
  if (tokens.length < 2) return false;

  if (!looksLikeClassList(trimmed)) return false;
  for (const token of tokens) {
    if (!classMap.has(token)) return false;
  }
  return true;
}

function rewriteInterpolationStringLiterals(
  code: string,
  classMap: Map<string, string>
): string {
  return code
    .replace(/"((?:\\.|[^"\\])*)"/g, (_match, inner: string) =>
      `"${rewriteClassTokens(inner, classMap)}"`
    )
    .replace(/'((?:\\.|[^'\\])*)'/g, (_match, inner: string) =>
      `'${rewriteClassTokens(inner, classMap)}'`
    );
}

function rewriteTemplateBody(body: string, classMap: Map<string, string>): string {
  let out = "";
  let i = 0;
  while (i < body.length) {
    const interpStart = body.indexOf("${", i);
    if (interpStart === -1) {
      out += rewriteClassTokens(body.slice(i), classMap);
      break;
    }
    out += rewriteClassTokens(body.slice(i, interpStart), classMap);
    const interpEnd = findMatchingBrace(body, interpStart + 2);
    if (interpEnd === -1) {
      out += body.slice(interpStart);
      break;
    }
    const interp = body.slice(interpStart + 2, interpEnd);
    out += "${" + rewriteInterpolationStringLiterals(interp, classMap) + "}";
    i = interpEnd + 1;
  }
  return out;
}

function rewriteJsxExpressionBody(body: string, classMap: Map<string, string>): string {
  let out = "";
  let i = 0;
  while (i < body.length) {
    const c = body[i];
    if (c === "\\") {
      out += body.slice(i, i + 2);
      i += 2;
      continue;
    }
    if (c === '"' || c === "'") {
      const quote = c;
      let j = i + 1;
      while (j < body.length) {
        if (body[j] === "\\") {
          j += 2;
          continue;
        }
        if (body[j] === quote) break;
        j++;
      }
      const inner = body.slice(i + 1, j);
      out += quote + rewriteClassTokens(inner, classMap) + quote;
      i = j + 1;
      continue;
    }
    if (c === "`") {
      let j = i + 1;
      while (j < body.length) {
        if (body[j] === "\\") {
          j += 2;
          continue;
        }
        if (body[j] === "`") break;
        if (body[j] === "$" && body[j + 1] === "{") {
          const closeIdx = findMatchingBrace(body, j + 2);
          if (closeIdx === -1) {
            j = body.length;
            break;
          }
          j = closeIdx + 1;
          continue;
        }
        j++;
      }
      const inner = body.slice(i + 1, j);
      out += "`" + rewriteTemplateBody(inner, classMap) + "`";
      i = j + 1;
      continue;
    }
    out += c;
    i++;
  }
  return out;
}

function rewriteJoinSpaceArrayClassStrings(code: string, classMap: Map<string, string>): string {
  let out = "";
  let lastEnd = 0;
  for (let i = 0; i < code.length; i++) {
    if (code[i] !== "[") continue;
    const closeIndex = findMatchingSquareBracket(code, i);
    if (closeIndex === -1) continue;
    if (!isSpaceJoinSuffix(code, closeIndex)) {
      i = closeIndex;
      continue;
    }

    out += code.slice(lastEnd, i + 1);
    out += rewriteJsxExpressionBody(code.slice(i + 1, closeIndex), classMap);
    lastEnd = closeIndex;
    i = closeIndex;
  }
  out += code.slice(lastEnd);
  return out;
}

function rewriteSourceClasses(code: string, classMap: Map<string, string>): string {
  code = code
    .replace(/className:\s*"([^"]*)"/g, (_match, literal: string) =>
      `className: "${rewriteClassTokens(literal, classMap)}"`
    )
    .replace(/className:\s*'([^']*)'/g, (_match, literal: string) =>
      `className: '${rewriteClassTokens(literal, classMap)}'`
    );

  code = code
    .replace(/className\s*=\s*"([^"]*)"/g, (_match, literal: string) =>
      `className="${rewriteClassTokens(literal, classMap)}"`
    )
    .replace(/className\s*=\s*'([^']*)'/g, (_match, literal: string) =>
      `className='${rewriteClassTokens(literal, classMap)}'`
    );

  code = rewriteJoinSpaceArrayClassStrings(code, classMap);

  code = code
    .replace(/"((?:\\.|[^"\\])*)"/g, (match, inner: string, offset: number) => {
      if (isLikelyModuleSpecifierString(code, offset)) return match;
      if (!shouldRewriteAsClassList(inner, classMap)) return match;
      return `"${rewriteClassTokens(inner, classMap)}"`;
    })
    .replace(/'((?:\\.|[^'\\])*)'/g, (match, inner: string, offset: number) => {
      if (isLikelyModuleSpecifierString(code, offset)) return match;
      if (!shouldRewriteAsClassList(inner, classMap)) return match;
      return `'${rewriteClassTokens(inner, classMap)}'`;
    });

  let out = "";
  let lastEnd = 0;
  const opener = /className\s*=\s*\{/g;
  let match: RegExpExecArray | null;
  while ((match = opener.exec(code)) !== null) {
    const bodyStart = match.index + match[0].length;
    out += code.slice(lastEnd, bodyStart);
    const bodyEnd = findMatchingBrace(code, bodyStart);
    if (bodyEnd === -1) {
      out += code.slice(bodyStart);
      lastEnd = code.length;
      break;
    }
    out += rewriteJsxExpressionBody(code.slice(bodyStart, bodyEnd), classMap) + "}";
    lastEnd = bodyEnd + 1;
    opener.lastIndex = lastEnd;
  }
  out += code.slice(lastEnd);

  return out;
}

function escapeRegex(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function escapeCssClassToken(value: string): string {
  // Tailwind-like selector escaping (sufficient for generated utility classes).
  return value.replace(/([^a-zA-Z0-9_-])/g, "\\$1");
}

// Convert CSS hex-escape sequences (\<1-6 hex digits> with an optional trailing
// space) to single-char backslash escapes (\<char>) for the printable ASCII
// chars that have a single-char escape form. Both forms are equivalent per
// CSS3 spec — they match the same character in a class attribute — but
// Tailwind v3+ emits the hex form for commas (\2c ), `#` (\23 ), etc., while
// our escapeCssClassToken above emits the single-char form (\,, \#). Without
// this normalisation, the rewriteSelectors regex never matches Tailwind-
// generated arbitrary-value selectors that contain commas/hashes/etc., so the
// CSS keeps the original Tailwind selector while the HTML emits the
// obfuscated class — a guaranteed unstyled element.
function normalizeCssEscapes(css: string): string {
  return css.replace(/\\([0-9a-fA-F]{1,6}) ?/g, (match, hex) => {
    const code = parseInt(hex, 16);
    if (code <= 0x20 || code > 0x7e) return match;
    const ch = String.fromCharCode(code);
    // Single-char backslash escape is only valid for non-alphanumerics; for
    // hex digits like \0061 ("a") we MUST keep the hex form or the selector
    // changes meaning.
    if (/[a-zA-Z0-9]/.test(ch)) return match;
    return "\\" + ch;
  });
}

function rewriteSelectors(selector: string, classMap: Map<string, string>): string {
  let rewritten = selector;
  const orderedEntries = Array.from(classMap.entries()).sort((left, right) => {
    const lengthDelta = right[0].length - left[0].length;
    if (lengthDelta !== 0) {
      return lengthDelta;
    }
    return left[0].localeCompare(right[0]);
  });

  for (const [token, mangled] of orderedEntries) {
    const escapedToken = escapeCssClassToken(token);
    const pattern = new RegExp(`\\.${escapeRegex(escapedToken)}(?![a-zA-Z0-9_-])`, "g");
    rewritten = rewritten.replace(pattern, `.${mangled}`);
  }
  return rewritten;
}

function rewriteCssClasses(css: string, classMap: Map<string, string>): string {
  // Normalise hex-escape sequences before parsing so all selectors use a
  // consistent single-char backslash escape form that rewriteSelectors can
  // match. See normalizeCssEscapes() for details.
  const root = postcss.parse(normalizeCssEscapes(css));
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
