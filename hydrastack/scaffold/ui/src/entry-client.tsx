import { hydrateRoot } from "react-dom/client";

import App from "./App";
import {
  attachRouteContract,
  ensureString,
  resolveHydraRoute,
  type JsonObject
} from "./routeContract";
import "./styles.css";

function readHydrationProps(): JsonObject {
  const payload = document.getElementById("__HYDRA_PROPS__");
  if (!payload || !payload.textContent) {
    return {};
  }

  try {
    return JSON.parse(payload.textContent) as JsonObject;
  } catch {
    return {};
  }
}

const root = document.getElementById("root");
if (root) {
  const requestUrl = `${window.location.pathname || "/"}${window.location.search || ""}`;
  const rawProps = readHydrationProps();
  const requestContext =
    typeof rawProps.__hydra_request === "object" && rawProps.__hydra_request !== null
      ? (rawProps.__hydra_request as JsonObject)
      : {};
  const route = resolveHydraRoute(requestUrl, rawProps, requestContext);
  let pageProps = attachRouteContract(rawProps, route);

  switch (route.pageId) {
    case "post_detail": {
      const postId = route.params.postId ?? ensureString(pageProps.postId, "");
      const postDetailMessageKey = ensureString(
        pageProps.messageKey,
        ensureString(pageProps.message_key, "post_detail_title")
      );
      pageProps = {
        ...pageProps,
        page: "post_detail",
        postId,
        messageKey: postDetailMessageKey
      };
      break;
    }
    case "home":
    default:
      const homeMessageKey = ensureString(
        pageProps.messageKey,
        ensureString(pageProps.message_key, "hello_from_hydrastack")
      );
      pageProps = {
        ...pageProps,
        page: "home",
        messageKey: homeMessageKey
      };
      break;
  }

  hydrateRoot(
    root,
    <App url={route.routeUrl} initialProps={pageProps} />
  );
}
