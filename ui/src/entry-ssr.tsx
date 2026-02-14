import { renderToString } from "react-dom/server";

import App from "./App";

declare global {
  interface Window {
    render?: (url: string, propsJson: string) => string;
  }

  interface GlobalThis {
    render?: (url: string, propsJson: string) => string;
  }
}

function escapeForScriptTag(value: string): string {
  return value.replace(/</g, "\\u003c").replace(/>/g, "\\u003e").replace(/&/g, "\\u0026");
}

function parseProps(propsJson: string): Record<string, unknown> {
  if (!propsJson) {
    return {};
  }

  try {
    return JSON.parse(propsJson) as Record<string, unknown>;
  } catch {
    return {};
  }
}

// SSR contract: globalThis.render(url, propsJson) -> full HTML document.
globalThis.render = (url: string, propsJson: string): string => {
  const props = parseProps(propsJson);
  const appHtml = renderToString(<App url={url || "/"} initialProps={props} />);
  const safeProps = escapeForScriptTag(JSON.stringify(props));

  return `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>HydraStack</title>
    <link rel="stylesheet" href="/assets/app.css" />
  </head>
  <body>
    <div id="root">${appHtml}</div>
    <script id="__HYDRA_PROPS__" type="application/json">${safeProps}</script>
    <script src="/assets/client.js" defer></script>
  </body>
</html>`;
};
