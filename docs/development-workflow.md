# AI-assisted development workflow (Stage 1)

This repository uses GitHub issues as the shared task queue, Codex CLI on the owner's Ubuntu workstation for implementation, and pull requests for review. The owner runs all hardware tests. **Nothing in this workflow schedules or starts Codex automatically.**

## Roles

- **Owner:** sets priorities, approves scope, operates bench hardware, accepts or rejects PRs, merges/releases.
- **Planning/review assistant:** reviews current repo, drafts specific GitHub issues, and reviews published branches/PRs. It cannot run commands on the owner's Ubuntu workstation from chat.
- **Codex CLI:** reads `AGENTS.md` and the selected issue; develops on a feature branch; builds/tests locally; submits a PR with evidence and hardware test steps.

## For each task

1. Agree on a scoped GitHub issue with acceptance criteria and explicit out-of-scope items.
2. From the local repo, sync `main` (`git status`, `git fetch`, `git pull --ff-only`) and create a feature branch. Do not overwrite uncommitted local work.
3. Launch Codex CLI **manually** inside the repo. Ask it to read `AGENTS.md`, inspect issue #N, implement *only that issue*, build, and produce a PR. If it cannot read GitHub issues in the local environment, paste the issue body.
4. Review the diff, build/test evidence, and PR description. Publish the feature branch/PR only when ready.
5. Ask the planning/review assistant to review the PR by number. Discuss fixes via the PR or a follow-up task.
6. Owner runs the stated physical hardware checks, records actual output/observations on the PR, and approves merge.
7. Only after approval: merge, update ROADMAP.md, and plan the next issue.

## Example Codex prompt

> Read AGENTS.md and the current repository source. Implement GitHub issue #1 only, on a new feature branch. Preserve current modem UART/SMS/portal functionality. Build using ESP-IDF v6.1.1; report exact build and test results. Prepare a pull request referencing #1, with design trade-offs, limitations, and explicit steps for my SIM7600 bench test. Do not merge, flash hardware, install firmware, or start another milestone without approval.

## Future automation boundary

A future local orchestrator can watch **explicitly approved** issues, invoke `codex exec`, run builds, and open PRs. It must have restricted credentials and isolated execution, log its actions, avoid duplicate job execution, and require human approval for merges, firmware releases, flashing, hardware control, and other external side effects.

No local automation service or GitHub Actions agent is installed by this Stage 1 setup.
