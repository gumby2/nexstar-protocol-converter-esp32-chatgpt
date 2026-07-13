# Extract module prompt

Work only in the currently opened repository. Follow `AGENTS.md`.

Branch: `{{branch_name}}`

Before editing:

- Confirm the current branch is `{{branch_name}}`.
- Confirm `git status --short` is clean.
- Do not modify `main` or `develop` unless this task explicitly says to.
- Do not perform unrelated cleanup or formatting.

Task:

Extract module: `{{module_name}}`

Source scope:

`{{source_scope}}`

Excluded scope:

`{{excluded_scope}}`

Files to create or update:

`{{files}}`

Required invariants:

`{{invariants}}`

Rules:

- Keep the change narrowly scoped to the requested extraction.
- Preserve existing protocol behavior, public endpoints, saved settings, Bluetooth behavior, SkySafari behavior, Wi-Fi, LX200, Alpaca, Stellarium, and web-interface behavior.
- Do not change `FW_VERSION` unless explicitly requested.
- Do not duplicate logic between the old and new module.
- Do not introduce circular dependencies.
- Preserve Arduino preprocessing compatibility.

Validation:

```sh
./scripts/test.sh
./scripts/build.sh
git diff --check
git status --short
```

Review the diff before committing.

Commit with exactly:

`{{commit_message}}`

Push the current branch and stop after the successful push.
