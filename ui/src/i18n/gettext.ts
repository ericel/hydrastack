type Catalog = Record<string, string>;

type LocaleBundle = {
  locale: string;
  localeCandidates: string[];
  gettext: (msgid: string) => string;
  _: (msgid: string) => string;
};

const FALLBACK_LOCALE = "en";

function normalizeLocale(locale: string): string {
  return locale.trim().replaceAll("_", "-").toLowerCase();
}

function decodePoLiteral(value: string): string {
  const trimmed = value.trim();
  if (!trimmed.startsWith("\"") || !trimmed.endsWith("\"")) {
    return "";
  }

  const quoted = trimmed.slice(1, -1);
  return quoted
    .replaceAll("\\\\", "\\")
    .replaceAll('\\"', '"')
    .replaceAll("\\n", "\n")
    .replaceAll("\\r", "\r")
    .replaceAll("\\t", "\t");
}

function parsePo(rawPo: string): Catalog {
  const catalog: Catalog = {};
  let currentMsgId: string[] = [];
  let currentMsgStr: string[] = [];
  let mode: "msgid" | "msgstr" | null = null;

  const flush = () => {
    const msgid = currentMsgId.join("");
    const msgstr = currentMsgStr.join("");
    if (msgid) {
      catalog[msgid] = msgstr || msgid;
    }
    currentMsgId = [];
    currentMsgStr = [];
    mode = null;
  };

  for (const line of rawPo.split(/\r?\n/)) {
    const trimmed = line.trim();

    if (!trimmed) {
      flush();
      continue;
    }

    if (trimmed.startsWith("#")) {
      continue;
    }

    if (trimmed.startsWith("msgid ")) {
      flush();
      currentMsgId.push(decodePoLiteral(trimmed.slice("msgid ".length)));
      mode = "msgid";
      continue;
    }

    if (trimmed.startsWith("msgstr ")) {
      currentMsgStr.push(decodePoLiteral(trimmed.slice("msgstr ".length)));
      mode = "msgstr";
      continue;
    }

    if (trimmed.startsWith("msgstr[0] ")) {
      currentMsgStr.push(decodePoLiteral(trimmed.slice("msgstr[0] ".length)));
      mode = "msgstr";
      continue;
    }

    if (trimmed.startsWith("msgctxt ") || trimmed.startsWith("msgid_plural ")) {
      continue;
    }

    if (trimmed.startsWith("\"")) {
      const segment = decodePoLiteral(trimmed);
      if (mode === "msgid") {
        currentMsgId.push(segment);
      } else if (mode === "msgstr") {
        currentMsgStr.push(segment);
      }
      continue;
    }
  }

  flush();
  return catalog;
}

function localeCandidateChain(locale: string): string[] {
  const normalized = normalizeLocale(locale);
  if (!normalized) {
    return [FALLBACK_LOCALE];
  }

  const candidates: string[] = [];
  let current = normalized;
  while (current) {
    if (!candidates.includes(current)) {
      candidates.push(current);
    }
    const separator = current.lastIndexOf("-");
    if (separator < 0) {
      break;
    }
    current = current.slice(0, separator);
  }

  if (!candidates.includes(FALLBACK_LOCALE)) {
    candidates.push(FALLBACK_LOCALE);
  }
  return candidates;
}

const RAW_CATALOG_FILES = import.meta.glob("../../locales/*/LC_MESSAGES/messages.po", {
  eager: true,
  query: "?raw",
  import: "default"
}) as Record<string, string>;

function extractLocaleFromPath(path: string): string {
  const match = path.match(/\/locales\/([^/]+)\/LC_MESSAGES\/messages\.po$/);
  if (!match || !match[1]) {
    return "";
  }
  return normalizeLocale(match[1]);
}

const CATALOGS: Record<string, Catalog> = {};
for (const [path, rawCatalog] of Object.entries(RAW_CATALOG_FILES)) {
  const locale = extractLocaleFromPath(path);
  if (!locale) {
    continue;
  }
  CATALOGS[locale] = parsePo(rawCatalog);
}

export function createGettext(locale: string): LocaleBundle {
  const localeCandidates = localeCandidateChain(locale);
  const resolvedLocale =
    localeCandidates.find((candidate) => CATALOGS[candidate]) ?? FALLBACK_LOCALE;
  const catalog = CATALOGS[resolvedLocale] ?? CATALOGS[FALLBACK_LOCALE] ?? {};

  const gettext = (msgid: string): string => catalog[msgid] ?? msgid;
  return {
    locale: resolvedLocale,
    localeCandidates,
    gettext,
    _: gettext
  };
}
