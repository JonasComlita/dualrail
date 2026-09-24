import type { MouseEvent, ReactNode } from "react";

export type SiteHeaderSection =
  | "overview"
  | "practice"
  | "learn"
  | "stack"
  | "research"
  | "evidence"
  | "arena"
  | "intelligence"
  | "workspaces"
  | "operations";

type SiteHeaderProps = {
  active?: SiteHeaderSection;
  brandTestId?: string;
  onNavigate?: (href: string) => void;
  intercept?: (href: string) => boolean;
  trailing?: ReactNode;
};

const NAV_ITEMS: Array<{ href: string; label: string; section: SiteHeaderSection }> = [
  { href: "/", label: "Overview", section: "overview" },
  { href: "/practice", label: "Practice", section: "practice" },
  { href: "/learn", label: "Learn", section: "learn" },
  { href: "/stack", label: "Stack", section: "stack" },
  { href: "/research", label: "Research", section: "research" },
  { href: "/evidence", label: "Evidence", section: "evidence" },
  { href: "/arena", label: "Arena", section: "arena" },
  { href: "/intelligence", label: "Intelligence", section: "intelligence" },
  { href: "/workspaces", label: "Workspaces", section: "workspaces" },
  { href: "/operations", label: "Operations", section: "operations" },
  { href: "/api/public/v1/openapi.json", label: "API", section: "overview" },
];

export function SiteHeader({ active, brandTestId, onNavigate, intercept, trailing }: SiteHeaderProps) {
  const navigate = (event: MouseEvent<HTMLAnchorElement>, href: string) => {
    if (!onNavigate || (intercept && !intercept(href))) return;
    event.preventDefault();
    onNavigate(href);
  };

  return (
    <header className="tc-universal-header">
      <a className="tc-universal-brand" href="/" data-testid={brandTestId} onClick={(event) => navigate(event, "/")}>
        TREATCODE
      </a>
      <nav className="tc-universal-nav" aria-label="Primary navigation">
        {NAV_ITEMS.map((item) => (
          <a
            key={item.href}
            className="tc-universal-link"
            href={item.href}
            data-testid={item.section === "arena" ? "arena-nav" : undefined}
            aria-current={active === item.section ? "page" : undefined}
            onClick={(event) => navigate(event, item.href)}
          >
            {item.label}
          </a>
        ))}
      </nav>
      {trailing ? <div className="tc-universal-trailing">{trailing}</div> : null}
    </header>
  );
}
