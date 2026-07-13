# Release prompt

Work only in the currently opened repository. Follow `AGENTS.md`.

Branch: `{{branch_name}}`

Before editing:

- Confirm the current branch is `{{branch_name}}`.
- Confirm `git status --short` is clean.
- Verify CI is green for the commit being released.
- Do not modify `main` or `develop` unless this task explicitly says to.
- Do not infer or increment a release version.

Explicit release version supplied by user:

`{{release_version}}`

Rules:

- Proceed only when `{{release_version}}` is explicitly provided.
- Update only intended versioned files.
- Do not perform unrelated cleanup or formatting.
- Build release artifacts.
- Verify release filenames follow `Nexstar_Protocol_Converter_vX.YY.ino`.
- Create a tag only when explicitly authorized.
- Create a GitHub Release only when explicitly authorized.

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
