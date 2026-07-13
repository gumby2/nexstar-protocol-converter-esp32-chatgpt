# Fix compile prompt

Work only in the currently opened repository. Follow `AGENTS.md`.

Branch: `{{branch_name}}`

Before editing:

- Confirm the current branch is `{{branch_name}}`.
- Confirm `git status --short` is clean.
- Do not modify `main` or `develop` unless this task explicitly says to.
- Do not perform unrelated cleanup or formatting.

Named change that introduced the compile error:

`{{named_change}}`

Exact compile command:

```sh
./scripts/build.sh 2>&1 | tee build.log
```

Reproduce the exact compile error before editing. Use focused log searches such as:

```sh
grep -i "error:" build.log
grep -i "undefined reference" build.log
grep -i "not declared" build.log
```

Rules:

- Fix only compile errors caused by `{{named_change}}`.
- Do not perform broad refactors.
- Preserve existing behavior, protocols, endpoints, and settings formats.
- Do not change `FW_VERSION` unless explicitly requested.
- Keep the diff narrow and review it before committing.

Validation:

```sh
./scripts/test.sh
./scripts/build.sh
git diff --check
git status --short
```

Commit with exactly:

`{{commit_message}}`

Push the current branch and stop after the successful push.
