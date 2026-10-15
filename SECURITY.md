# Security Policy

## Supported versions

Beecord is in early development (pre-M0) and has no tagged releases yet.
Security fixes land on the `main` branch only.

| Version | Supported |
|---|---|
| `main` branch | ✅ |
| anything older | ❌ (no older releases exist) |

## Reporting a vulnerability

**Please do not open a public issue for security vulnerabilities.**

Report them privately via GitHub's
[Private Vulnerability Reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability):

**[Report a vulnerability → scottli139/beecord](https://github.com/scottli139/beecord/security/advisories/new)**

(Repository → Security → Advisories → "Report a vulnerability".)

Include as much as you can: affected component/commit, steps to reproduce,
impact, and any suggested fix. Do not include real recording data, personal
information, or live API keys in the report.

## What to expect

- We will acknowledge your report **within 7 days** (best effort — the team is
  small and pre-launch; we ask for patience).
- We will work with you to confirm the issue, develop a fix on a private
  branch/fork, and agree on an embargo period.
- Once a fix is merged, we will publish a
  [GitHub security advisory](https://github.com/scottli139/beecord/security/advisories)
  crediting you (unless you prefer to stay anonymous) and describing the
  vulnerability and the fix.
- If a report turns out not to be a vulnerability, we will explain why and
  usually convert it into a public issue (with your permission).

If you cannot use GitHub's private reporting for some reason, open a public
issue *without* exploit details and ask the maintainer to enable contact — but
the advisory route above is strongly preferred.

## Sensitive surfaces in this project

When assessing impact (and when redacting reports/logs), keep these in mind:

1. **BLE pairing / binding token** — BuzzLink uses a pairing token to bind an
   app to a device. Leaks or bypasses of the pairing/binding flow let an
   attacker sync from or impersonate a device.
2. **User-configured AI API keys** — Beecord Studio stores the OpenAI-compatible
   endpoint and API key the user configures for cloud summaries (Kimi,
   DeepSeek, ...). The key must stay on the user's machine; it must never be
   logged, transmitted elsewhere, or committed.
3. **Recording contents** — Recordings are personal data. Paths to issues:
   unencrypted sync transport, over-broad file exposure in the app, cloud-AI
   features sending content without explicit opt-in, and anything that writes
   real audio into logs or the repository.

Anything touching these surfaces gets priority triage.

## Scope notes

- The BuzzLinkSim virtual device is a development tool; vulnerabilities that
  only affect Sim (not real firmware/Studio usage) are still worth reporting,
  but may be handled as regular bugs.
- Out of scope: attacks that require physical access to an unlocked Mac,
  jailbroken iOS, or a compromised user account.
