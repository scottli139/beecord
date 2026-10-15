# Contributing to Beecord

Thanks for your interest in Beecord! Contributions of code, docs, design, and
issue reports are all welcome.

> **Project stage**: early development (pre-M0). Things move fast, interfaces
> change, and not everything described in `docs/` is implemented yet. Small,
> focused PRs are much easier to merge than large ones — when in doubt, open an
> issue first and ask.

## Ground rules

- Be kind and constructive. See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
- Follow [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) — it applies to
  human and AI contributors equally.
- Follow [docs/TESTING.md](docs/TESTING.md) for what tests a change needs.
- Never commit secrets, API keys, certificates, personal data, or real
  recordings (synthetic test samples only — see CODING_STANDARDS §1.5).
- Never commit third-party trademarks, proprietary protocol constants, or
  binary material you have no right to distribute. This repository must stay
  self-contained and independently implementable.

## Getting oriented

| Path | Content |
|---|---|
| [docs/BUZZLINK.md](docs/BUZZLINK.md) | BuzzLink BLE file-sync protocol spec — the protocol behavior source of truth |
| [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) | Language, style, and compliance rules |
| [docs/TESTING.md](docs/TESTING.md) | Test layers, gates, and the no-hardware workflow |
| [apps/mac/BuzzLinkSim/](apps/mac/BuzzLinkSim/) | Virtual device app used as the development peer |
| [firmware/main/proto/](firmware/main/proto/) | Hardware-independent protocol layer (C) |

## No hardware? No problem

You do **not** need the ESP32-S3 watch board to contribute. The BuzzLinkSim
virtual device app (macOS + iOS/iPadOS) implements the device side of the
protocol, with fault injection for error paths:

- **Loopback TCP** (`127.0.0.1:40404`, enabled by default in Sim) — same-machine
  development and UITests. CoreBluetooth cannot discover a peripheral
  advertising from the same Mac, so Sim provides a local byte stream that is
  isomorphic to the BLE link (same frame protocol).
- **BLE** — run Sim on a second Mac or an iPhone/iPad as a real over-the-air
  peer for Beecord Studio.

Firmware protocol-layer code (`firmware/main/proto/`) is deliberately free of
ESP-IDF dependencies and builds/runs as host unit tests on macOS — see the
third gate below.

## Test gates (must pass before submitting)

Three commands, all runnable without hardware:

```bash
# Gate 1 — BuzzLinkCore unit tests (SwiftPM package, independent of the Xcode project)
cd apps/mac/Packages/BuzzLink && swift test

# Gate 2 — Studio / BuzzLinkSim build + UI tests (Xcode project)
cd apps/mac && xcodebuild test -scheme BeecordStudio -destination 'platform=macOS'

# Gate 3 — firmware protocol layer host unit tests (C, gcc/clang)
cd firmware/test_host && make test
```

Protocol-layer changes without tests are not merged (TESTING.md §6). If your
change affects protocol behavior, update `docs/BUZZLINK.md` and the JSON test
vectors **first**, then the implementation (TESTING.md §3).

## Pull request process

1. **Fork** the repository and create a topic branch from `main`
   (`git checkout -b add-xyz`).
2. Keep commits small and single-purpose; write commit messages in English,
   imperative mood (`Add frame reassembly`), explaining *why* in the body when
   it isn't obvious (CODING_STANDARDS §6).
3. Make sure the test gates above pass locally.
4. Open a PR against `main` and fill in the PR template.
5. Respond to review feedback; maintainers will merge when green.

## Developer Certificate of Origin (DCO)

Every commit must carry a `Signed-off-by` line certifying the DCO 1.1
(developercertificate.org). Use `git commit -s` (configure `user.name` and
`user.email` first):

```bash
git commit -s -m "Add frame reassembly"
```

If multiple people author a commit, add their sign-offs too. The DCO is a
lightweight declaration that you have the right to submit the contribution —
no CLA is required. By submitting a contribution, you agree to license it under
the same terms as the project: [Apache-2.0](LICENSE).

Full text of DCO 1.1 (<https://developercertificate.org/>):

> Developer's Certificate of Origin 1.1
>
> By making a contribution to this project, I certify that:
>
> (a) The contribution was created in whole or in part by me and I
>     have the right to submit it under the open source license
>     indicated in the file; or
>
> (b) The contribution is based upon previous work that, to the best
>     of my knowledge, is covered under an appropriate open source
>     license and I have the right under that license to submit that
>     work with modifications, whether created in whole or in part
>     by me, under the same open source license (unless I am
>     permitted to submit under a different license), as indicated
>     in the file; or
>
> (c) The contribution was provided directly to me by some other
>     person who certified (a), (b) or (c) and I have not modified
>     it.
>
> (d) I understand and agree that this project and the contribution
>     are public and that a record of the contribution (including all
>     personal information I submit with it, including my sign-off) is
>     maintained indefinitely and may be redistributed consistent with
>     this project or the open source license(s) involved.
