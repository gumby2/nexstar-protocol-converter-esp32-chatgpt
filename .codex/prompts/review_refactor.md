# Review refactor prompt

Work only in the currently opened repository. Follow `AGENTS.md`.

Branch: `{{branch_name}}`

Before reviewing:

- Confirm the current branch is `{{branch_name}}`.
- Confirm `git status --short` is clean.
- Confirm CI status for the refactor branch when available.
- Do not modify code unless this task explicitly requests fixes.
- Do not modify `main` or `develop` unless this task explicitly says to.

Review scope:

`{{review_scope}}`

Inspect:

- ownership boundaries
- duplicated logic
- API surface
- behavior preservation
- protocol invariants
- CI and validation status

Protocol invariants include the original NexStar single-command rule, required `?`/`#` handshake, `@` completion handling, no `E`/`Z` polling during active GOTO, cached or estimated responses during GOTO, unsupported AbortSlew, and existing big-endian coordinate encoding.

Return findings ordered by severity. Cite file paths and line numbers where possible. If there are no findings, say so and list any residual test or CI risk.

Required validation status to report:

```sh
./scripts/test.sh
./scripts/build.sh
git diff --check
git status --short
```

If fixes are explicitly requested, review the diff, commit with exactly:

`{{commit_message}}`

Push the current branch and stop after the successful push.
