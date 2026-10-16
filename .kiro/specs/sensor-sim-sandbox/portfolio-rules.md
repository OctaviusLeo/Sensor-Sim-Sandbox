---
inclusion: always
---
# Portfolio project rules

This repository is portfolio work for simulation, test and autonomy software roles. Interviewers will ask me to explain and change any part of it, so I must understand every line.

## Results must be real
- Every number in the README or docs (rates, errors, success rates, timings) comes from a script in this repo. Record the command, seed and commit hash next to it. Never type, estimate or borrow a result.
- If a run fails or misses a target, report the real result and the cause. Never loosen a test or threshold just to make it pass.
- Results tables are generated files (for example results/summary.md), not hand edits.

## Ownership and confidentiality
- Personal computer and personal accounts only. Never use code, data, documents, tool names or system names from any employer or client, and never mention an employer or client in this repo.
- The repository stays private until I say otherwise: no public license, no package publishing, no public releases.
- Check the license of every third-party package and asset before adding it, and list it in THIRD_PARTY.md. Do not commit marketplace or engine content whose license forbids redistribution.
- No secrets in the repo. Use environment variables and .gitignore.

## Engineering standards
- Before relying on a package, plugin, API or version, prove it works on this machine (package manager query, docs for that exact version, or a minimal test) and record exact versions in docs/ENVIRONMENT.md. If something is uncertain, say so and propose a check; do not guess.
- One command reproduces each result. Pin dependency versions. Fix seeds wherever randomness is involved.
- Unit tests for every non-trivial module. The test command is in the README, and CI runs it where the toolchain allows.
- Small commits, one task per commit where practical, with clear messages.

## How to work with me
- Follow the spec workflow: requirements, then design, then tasks.
- When I start "Run all tasks", work through the whole task list without stopping for my review between milestones. Each milestone's automated gate in tasks.md replaces the review stop. Implement every task yourself; do not mark tasks [ME].
- After each task, report: what changed, how it was verified (commands and their output), and the one or two concepts I should be able to explain in an interview.
