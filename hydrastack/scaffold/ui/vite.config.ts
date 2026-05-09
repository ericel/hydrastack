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

function extractClassLiterals(code: string): string[] {
  const literals: string[] = [];

  // JSX attribute string forms.
  for (const m of code.matchAll(/className\s*=\s*"([^"]*)"/g)) {
    if (m[1]) literals.push(m[1]);
  }
  for (const m of code.matchAll(/className\s*=\s*'([^']*)'/g)) {
    if (m[1]) literals.push(m[1]);
  }
  // Object-property syntax.
  for (const m of code.matchAll(/className:\s*"([^"]*)"/g)) {
    if (m[1]) literals.push(m[1]);
  }
  for (const m of code.matchAll(/className:\s*'([^']*)'/g)) {
    if (m[1]) literals.push(m[1]);
  }

  // JSX `className={...}` expressions: anything inside the braces. We walk the
  // expression body and harvest every string literal and every template
  // literal we encounter. Covers `{"foo"}`, `{'foo'}`, `` {`foo ${...}`} ``,
  // `{[a, b ? "x" : "y"].join(" ")}`, `{cn("foo")}`, `{cond ? "a" : "b"}`, etc.
  // Without this generalisation the previous narrow regexes silently skipped
  // anything but the trivial cases — leaving real Tailwind utility classes
  // out of the classMap and therefore also out of the obfuscated CSS.
  const opener = /className\s*=\s*\{/g;
  let m: RegExpExecArray | null;
  while ((m = opener.exec(code)) !== null) {
    const bodyStart = m.index + m[0].length;
    const bodyEnd = findMatchingBrace(code, bodyStart);
    if (bodyEnd === -1) break;
    extractFromExpressionBody(code.slice(bodyStart, bodyEnd), literals);
    opener.lastIndex = bodyEnd + 1;
  }

  return literals;
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

// Rewrite string literals inside a `${...}` block — by convention, anything
// quoted inside a `className={\`...\`}` interpolation is a class-token list, so
// we rewrite its tokens. Non-string expressions (function calls, identifiers,
// etc.) flow through unchanged because rewriteClassTokens only matches \S+
// inside the captured string body.
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

// Find the matching '}' for an opening '${' starting at openIndex (0-based,
// pointing at the character AFTER the '{'). Returns the index of the matching
// '}' or -1 if not found. Skips over string and (best-effort) template-literal
// contents so braces inside strings don't disrupt counting.
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

// Rewrite the body of a template literal (the substring between the surrounding
// backticks) used as a className value. Walks the body and:
//   • static segments (text outside of ${...}) → rewriteClassTokens
//   • ${...} interpolations → rewriteInterpolationStringLiterals on the body,
//     so things like `${isOpen ? "translate-x-0" : "-translate-x-full"}` get
//     their inner string literals obfuscated too.
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

// Walk a JSX expression body inside `className={...}` and rewrite every
// recognisable class-token source: bare string literals, template literals
// (with interpolations), and nested string literals inside any expression.
// Identifiers, function calls, member accesses etc. flow through untouched.
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

function rewriteSourceClasses(code: string, classMap: Map<string, string>): string {
  // Non-JSX object-property syntax (e.g. `{ className: "foo" }`).
  code = code
    .replace(/className:\s*"([^"]*)"/g, (_match, literal: string) =>
      `className: "${rewriteClassTokens(literal, classMap)}"`
    )
    .replace(/className:\s*'([^']*)'/g, (_match, literal: string) =>
      `className: '${rewriteClassTokens(literal, classMap)}'`
    );

  // JSX attribute string forms.
  code = code
    .replace(/className\s*=\s*"([^"]*)"/g, (_match, literal: string) =>
      `className="${rewriteClassTokens(literal, classMap)}"`
    )
    .replace(/className\s*=\s*'([^']*)'/g, (_match, literal: string) =>
      `className='${rewriteClassTokens(literal, classMap)}'`
    );

  // JSX `className={...}` expressions — anything inside the braces.
  // Previously this was a list of narrow regex patterns (one for `{"foo"}`,
  // one for `{`foo`}`, etc.) that silently skipped the common cases:
  //   • template literals with ${...} interpolations
  //   • array-join patterns: className={[ "foo", cond && "bar" ].join(" ")}
  //   • function calls: className={cn("foo", { active: x })}
  //   • ternaries:       className={cond ? "foo" : "bar"}
  // All of which are common in real React+Tailwind code and were leaking raw
  // class names into the SSR HTML. Now we walk the expression body once with
  // findMatchingBrace and rewrite every string literal / template literal we
  // encounter — string contents are treated as class-token lists by convention.
  let out = "";
  let lastEnd = 0;
  const opener = /className\s*=\s*\{/g;
  let m: RegExpExecArray | null;
  while ((m = opener.exec(code)) !== null) {
    const bodyStart = m.index + m[0].length;
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
