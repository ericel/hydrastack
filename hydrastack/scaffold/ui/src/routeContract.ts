export type JsonObject = Record<string, unknown>;

export type HydraRoute = {
  pageId: string;
  params: Record<string, string>;
  query: Record<string, string>;
  routePath: string;
  routeUrl: string;
};

function asObject(value: unknown): JsonObject {
  if (typeof value !== "object" || value === null) {
    return {};
  }
  return value as JsonObject;
}

function asString(value: unknown, fallback = ""): string {
  return typeof value === "string" ? value : fallback;
}

function asStringRecord(value: unknown): Record<string, string> {
  const objectValue = asObject(value);
  const out: Record<string, string> = {};

  for (const [key, raw] of Object.entries(objectValue)) {
    if (typeof raw === "string") {
      out[key] = raw;
    } else if (typeof raw === "number" || typeof raw === "boolean") {
      out[key] = String(raw);
    }
  }

  return out;
}

export function resolveHydraRoute(
  fallbackUrl: string,
  props: JsonObject,
  requestContext: JsonObject = {}
): HydraRoute {
  const route = asObject(props.__hydra_route);
  const fallbackPath = asString(requestContext.routePath, asString(props.path, "/")) || "/";
  const fallbackRouteUrl =
    asString(requestContext.routeUrl, fallbackUrl || fallbackPath) || fallbackPath;
  const routePath = asString(route.routePath, fallbackPath) || "/";
  const routeUrl = asString(route.routeUrl, fallbackRouteUrl) || routePath;

  return {
    pageId: asString(route.pageId, asString(props.page, "home")) || "home",
    params: asStringRecord(route.params),
    query: asStringRecord(route.query),
    routePath,
    routeUrl
  };
}

export function attachRouteContract(props: JsonObject, route: HydraRoute): JsonObject {
  return {
    ...props,
    __hydra_route: route,
    page: route.pageId
  };
}

export function ensureString(value: unknown, fallback = ""): string {
  return asString(value, fallback);
}
