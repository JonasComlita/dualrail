import React, { Suspense } from "react";
import ReactDOM from "react-dom/client";
import "./index.css";

const normalizedPath = window.location.pathname.replace(/\/index\.html$/, "").replace(/\/$/, "") || "/";
const isOperationsRoute = normalizedPath === "/operations" || normalizedPath.startsWith("/operations/");
const isWorkspaceRoute = normalizedPath === "/workspaces" || normalizedPath.startsWith("/workspaces/");
const isImplementationArenaRoute = normalizedPath === "/arena" || normalizedPath.startsWith("/arena/");
const isIntelligenceRoute = normalizedPath === "/intelligence" || normalizedPath.startsWith("/intelligence/");
const isPublicReadingRoute =
  normalizedPath === "/" ||
  normalizedPath === "/stack" ||
  normalizedPath.startsWith("/stack/") ||
  normalizedPath === "/learn" ||
  normalizedPath.startsWith("/learn/") ||
  normalizedPath === "/search";

const RouteApp = isImplementationArenaRoute
  ? React.lazy(() => import("./ImplementationArena"))
  : isIntelligenceRoute
  ? React.lazy(() => import("./IntelligenceApp"))
  : isOperationsRoute
  ? React.lazy(() => import("./OperationsApp"))
  : isWorkspaceRoute
  ? React.lazy(() => import("./WorkspaceApp"))
  : isPublicReadingRoute
  ? React.lazy(() => import("./PublicApp"))
  : React.lazy(() => import("./App"));

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <Suspense fallback={<div aria-live="polite" style={{ padding: 24, fontFamily: "system-ui" }}>Loading TreatCode…</div>}>
      <RouteApp />
    </Suspense>
  </React.StrictMode>
);
