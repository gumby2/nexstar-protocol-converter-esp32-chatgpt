# Add feature prompt

Work only in the currently opened repository. Follow `AGENTS.md`.

Branch: `{{branch_name}}`

Before editing:

- Confirm the current branch is `{{branch_name}}`.
- Confirm `git status --short` is clean.
- Do not modify `main` or `develop` unless this task explicitly says to.
- Do not perform unrelated cleanup or formatting.

Feature description:

`{{feature_description}}`

Accepted behavior:

`{{accepted_behavior}}`

Excluded behavior:

`{{excluded_behavior}}`

Affected files:

`{{affected_files}}`

Required tests:

`{{tests}}`

Rules:

- Keep the change narrowly scoped to the requested feature.
- Preserve all existing protocols and endpoints unless this task explicitly says otherwise.
- Preserve Bluetooth behavior, SkySafari behavior, Wi-Fi, LX200, Alpaca, Stellarium, and web-interface behavior.
- Do not change saved-settings formats without migration handling.
- Do not change `FW_VERSION` unless explicitly requested.
- Do not perform unrelated cleanup or formatting.

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
