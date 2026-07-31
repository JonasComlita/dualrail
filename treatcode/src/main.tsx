import React, { Suspense } from "react";
import ReactDOM from "react-dom/client";
import "./index.css";

const normalizedPath = window.location.pathname.replace(/\/index\.html$/, "").replace(/\/$/, "") || "/";
const isPublicReadingRoute =
  normalizedPath === "/" ||
  normalizedPath === "/stack" ||
  normalizedPath.startsWith("/stack/") ||
  normalizedPath === "/learn" ||
  normalizedPath.startsWith("/learn/") ||
  normalizedPath === "/search";

const RouteApp = isPublicReadingRoute
  ? React.lazy(() => import("./PublicApp"))
  : React.lazy(() => import("./App"));

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <Suspense fallback={<div aria-live="polite" style={{ padding: 24, fontFamily: "system-ui" }}>Loading TreatCode…</div>}>
      <RouteApp />
    </Suspense>
  </React.StrictMode>
);
