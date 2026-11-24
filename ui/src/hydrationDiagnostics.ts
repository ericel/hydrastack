// Hydration mismatch reporting for hydrateRoot's onRecoverableError.
//
// When the server HTML and the client's first render differ, React recovers
// by re-rendering on the client and reports a recoverable error. Production
// builds report only minified codes (#418, #423), with no page or component.
// This reporter adds the route and component stack, keeps the last few
// reports on window.__HYDRA_HYDRATION_ERRORS__ for tests and QA tooling, and
// then hands the error to the browser's normal error reporting, as React's
// default handler does.

export type HydrationErrorReport = {
  route: string;
  message: string;
  digest?: string;
  componentStack?: string;
  at: number;
};

type RecoverableErrorInfo = {
  componentStack?: string | null;
  digest?: string | null;
};

declare global {
  interface Window {
    __HYDRA_HYDRATION_ERRORS__?: HydrationErrorReport[];
  }
}

const MAX_REPORTS = 20;

function errorMessage(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

export function createHydrationErrorReporter(
  route: string,
  options: { isDevelopment?: boolean; report?: (error: unknown) => void } = {}
) {
  const isDevelopment = options.isDevelopment ?? import.meta.env.DEV;
  const report =
    options.report ??
    ((error: unknown) => {
      if (typeof globalThis.reportError === "function") {
        globalThis.reportError(error);
      } else {
        console.error(error);
      }
    });

  return (error: unknown, errorInfo?: RecoverableErrorInfo) => {
    const entry: HydrationErrorReport = {
      route,
      message: errorMessage(error),
      at: Date.now()
    };
    if (errorInfo?.digest) {
      entry.digest = errorInfo.digest;
    }
    if (errorInfo?.componentStack) {
      entry.componentStack = errorInfo.componentStack;
    }

    if (typeof window !== "undefined") {
      const reports = (window.__HYDRA_HYDRATION_ERRORS__ ??= []);
      reports.push(entry);
      if (reports.length > MAX_REPORTS) {
        reports.splice(0, reports.length - MAX_REPORTS);
      }
    }

    if (isDevelopment) {
      console.error(
        `[hydra] Hydration mismatch on ${route}: the server HTML differs from the client's first render. ` +
          "Look for browser-only values (window, Date.now(), Math.random(), locale or time zone) " +
          "or globals the SSR runtime does not provide (see docs/ssr-runtime.md).\n" +
          entry.message +
          (entry.componentStack ? `\nComponent stack:${entry.componentStack}` : "")
      );
    }
    report(error);
  };
}
