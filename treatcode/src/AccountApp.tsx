import { useEffect, useState, type FormEvent } from "react";
import { SiteHeader } from "./SiteHeader";
import "./account.css";

const TOKEN_KEY = "treatcode.auth.token";

export default function AccountApp() {
  const [mode, setMode] = useState<"login" | "register">(() => new URLSearchParams(window.location.search).get("mode") === "register" ? "register" : "login");
  const [token, setToken] = useState(() => window.localStorage.getItem(TOKEN_KEY) || "");
  const [identity, setIdentity] = useState("");
  const [handle, setHandle] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    if (!token) return;
    const controller = new AbortController();
    void fetch("/api/auth/v1/capabilities", { headers: { Authorization: `Bearer ${token}` }, signal: controller.signal })
      .then(async (response) => {
        if (!response.ok) {
          if (response.status === 401 || response.status === 403) {
            window.localStorage.removeItem(TOKEN_KEY);
            setToken("");
          }
          throw new Error("Unable to load your account. Please sign in again.");
        }
        const payload = await response.json();
        setIdentity(payload.data?.principal?.handle || payload.data?.principal?.display_name || "Signed in");
      })
      .catch((reason) => { if (!controller.signal.aborted) setError(String(reason.message || reason)); });
    return () => controller.abort();
  }, [token]);

  async function authenticate(event: FormEvent) {
    event.preventDefault();
    setBusy(true);
    setError("");
    try {
      const response = await fetch(`/api/auth/v1/participants/${mode}`, {
        method: "POST",
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({ handle: handle.trim(), password }),
      });
      const payload = await response.json();
      if (!response.ok) throw new Error(payload.error?.reason || "Unable to sign in.");
      const nextToken = payload.data?.credential?.token;
      if (typeof nextToken !== "string" || !nextToken) throw new Error("The account service did not return a session.");
      window.localStorage.setItem(TOKEN_KEY, nextToken);
      setToken(nextToken);
      setIdentity(payload.data?.identity?.handle || handle.trim());
      setPassword("");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Account service unavailable.");
    } finally {
      setBusy(false);
    }
  }

  function logout() {
    window.localStorage.removeItem(TOKEN_KEY);
    setToken("");
    setIdentity("");
    setError("");
  }

  return <>
    <SiteHeader />
    <main className="account-main">
      <h1>Your TreatCode account</h1>
      <p>Save your practice solutions and take part in discussions.</p>
      {token ? <section className="account-panel" aria-label="Active account">
        <h2>{identity || "Loading account…"}</h2>
        <a href="/practice">Continue to Practice</a>
        <button type="button" onClick={logout}>Sign out</button>
      </section> : <form className="account-panel" onSubmit={authenticate}>
        <div className="account-actions" aria-label="Account actions">
          <button type="button" aria-pressed={mode === "login"} onClick={() => { setMode("login"); setError(""); }} disabled={busy}>Log in</button>
          <button type="button" aria-pressed={mode === "register"} onClick={() => { setMode("register"); setError(""); }} disabled={busy}>Sign up</button>
        </div>
        <label htmlFor="account-handle">Handle<input id="account-handle" value={handle} onChange={(event) => setHandle(event.target.value)} autoComplete="username" minLength={3} maxLength={24} required disabled={busy} /></label>
        <label htmlFor="account-password">Password<input id="account-password" type="password" value={password} onChange={(event) => setPassword(event.target.value)} autoComplete={mode === "register" ? "new-password" : "current-password"} minLength={mode === "register" ? 12 : undefined} maxLength={128} required disabled={busy} /></label>
        {mode === "register" ? <p className="account-help">Use a 3–24 character handle and a password with at least 12 characters.</p> : null}
        <button className="account-submit" type="submit" disabled={busy}>{busy ? "Working…" : mode === "register" ? "Create account" : "Log in"}</button>
      </form>}
      {error ? <p role="alert" className="account-error">{error}</p> : null}
    </main>
  </>;
}
