import { hydrateRoot } from "react-dom/client";

import App from "./App";
import "./styles.css";

function readHydrationProps(): Record<string, unknown> {
  const payload = document.getElementById("__HYDRA_PROPS__");
  if (!payload || !payload.textContent) {
    return {};
  }

  try {
    return JSON.parse(payload.textContent) as Record<string, unknown>;
  } catch {
    return {};
  }
}

const root = document.getElementById("root");
if (root) {
  hydrateRoot(
    root,
    <App
      url={`${window.location.pathname || "/"}${window.location.search || ""}`}
      initialProps={readHydrationProps()}
    />
  );
}
