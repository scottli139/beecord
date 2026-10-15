<!-- Thanks for the PR! Please fill this out — it makes review much faster. -->

## Summary

<!-- What does this PR change and why? If it fixes an issue, use "Fixes #123". -->

## Test evidence

All three gates passed locally (see [docs/TESTING.md](docs/TESTING.md)):

```bash
cd apps/mac/Packages/BuzzLink && swift test
cd apps/mac && xcodebuild test -scheme BeecordStudio -destination 'platform=macOS'
cd firmware/test_host && make test
```

<!-- Paste the tail of each command's output, or note which gates are not applicable and why. -->

## Checklist

- [ ] All applicable test gates pass (output above)
- [ ] Protocol/design docs updated if behavior changed (`docs/BUZZLINK.md`, design docs)
- [ ] Every commit includes `Signed-off-by` (`git commit -s` — DCO, see [CONTRIBUTING.md](CONTRIBUTING.md))
- [ ] No secrets, API keys, personal data, or real recordings included (synthetic samples only)
- [ ] No third-party trademarks, proprietary protocol constants, or binary material without rights
- [ ] New/changed logic has tests; protocol-layer changes include test vectors (TESTING.md §3/§6)
