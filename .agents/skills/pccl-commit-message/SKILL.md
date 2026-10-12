---
name: pccl-commit-message
description: Generate or update commit messages for the PCCL project following CONTRIBUTING.md (concise imperative titles, DCO sign-offs, and optional GitHub issue references). Use when the user mentions commit, commit message, amend, GitHub pull requests, or asks for help writing or updating a commit message in any language.
---

# PCCL Commit Message Generator

Generate or update Git commit messages for the PCCL GitHub workflow.
Follow the commit message and DCO requirements in [CONTRIBUTING.md](../../../CONTRIBUTING.md).

## Trigger

Activate when the user:
- Asks to create or update a commit message
- Mentions "commit" or "commit message", or equivalent terms in another language
- Explicitly says "create commit" or "update commit"

## Determine Mode: Create vs Amend

- **Create**: There are staged or unstaged changes that need a new commit.
- **Amend**: The most recent commit message needs to be revised (typically before pushing or after amend).

Mode is determined interactively in Workflow Step 2. If user's intent is already clear (e.g., "Update my commit message"), skip Step 2.

## Commit Message Format

```
<type>: <short summary in imperative mood>

<problem, solution, and any relevant limitations>

[optional: Refs: #<issue_number>]
[for breaking changes: explain the impact and migration in the body]
Signed-off-by: Your Name <your@email.com>
```

### Rules

1. **Header**: Default to a suitable type prefix using the guidance below. If the change is difficult to classify, omit the prefix rather than forcing a category. Write the summary in English imperative mood and keep the full title, including any prefix, within 50 characters. Prefixes are recommended, not required: accept valid unprefixed titles and do not add a prefix to an existing title solely for formatting unless requested.
2. **Body**: Explain the problem, solution, and any relevant limitations in English. Separate the body from the header by one blank line.
3. **GitHub Issues (optional)**: Reference a real, relevant GitHub issue with `Refs: #<number>`. Use `Closes #<number>` or `Fixes #<number>` only when the patch resolves the issue and automatic closure is intended; use `owner/repo#<number>` for another repository. Reference related issues when applicable; omit the reference if none applies. A missing issue reference does not block committing or pushing. Never invent a reference.

4. **Breaking changes**: Explain the impact and migration.
5. **DCO sign-off**: Include the contributor's authorized `Signed-off-by` line. Preserve existing `Signed-off-by` and `Co-authored-by` lines when amending or squashing. See CONTRIBUTING.md for details.

### Type Prefixes

Choose a prefix based on the main purpose of the change:

| Prefix | Purpose |
| --- | --- |
| `feat` | Add functionality |
| `fix` | Correct a bug |
| `docs` | Update documentation |
| `test` | Add or update tests |
| `refactor` | Restructure code without changing behavior |
| `perf` | Improve performance |
| `chore` | Maintain build tooling, configuration, or dependencies |

These are generation guidelines, not a required classification scheme. If no category clearly fits, use a concise unprefixed title.

### Example

```
fix: Prevent races in uint64 loads and stores

- replace all normal volatile uint64 load/store with b32x2 intrinsic in kernel code
- add volatile decorator in net.cc for head/tail(uint64) load/store
- change ncclSteps type from int to uint64_t in prims_simple.h

Closes #123: glm/gpt training hang
Signed-off-by: Your Name <your@email.com>
```

## Workflow

### Step 1: Check for Unstaged Changes

Run `git status` to check for unstaged changes:
- If there are unstaged files, use `AskUserQuestion` to ask whether to include them in the commit
- If no unstaged changes exist, proceed to Step 2

### Step 2: Determine Commit Mode

Use `AskUserQuestion` to ask the user whether they want to **create** a new commit or **amend** the existing one:
- **Create**: New commit with staged/unstaged changes
- **Amend**: Modify the most recent commit message

If user's intent is already clear (e.g., "Update my commit message"), skip this step.

### Step 3: Gather Diff

- **Create mode**: Run `git diff --cached` (staged). If empty, run `git diff` (unstaged).
- **Amend mode**: Run `git log -1 --format="%B"` to get existing message and `git diff HEAD~1` to see changes.

### Step 4: Extract Existing Metadata (Amend Mode Only)

From the existing commit message, extract and preserve:
- Relevant GitHub issue references, if present
- Existing breaking-change markers and migration details, checking that they still match the full change
- Existing `Co-authored-by` and `Signed-off-by` lines

### Step 5: Collect Missing Information via AskUserQuestion

Use `AskUserQuestion` tool to interactively gather required information:

**For Create Mode:**
1. **Files to include**: If unstaged files exist, ask which files to include
2. **GitHub Issue (optional)**: Verify an issue number or URL when one is supplied or relevant. If none applies, proceed without an issue reference.

**For Amend Mode:**
1. Run `git diff HEAD~1` to get all changes since last commit
2. **Automatically generate new commit message** based on the full diff (do NOT ask user which fields to update)
3. **Present to user** for review with before/after comparison
4. **If user requests changes**: use `AskUserQuestion` to ask which specific field needs updating, then modify only that field

### Step 6: Generate Commit Message

Based on the diff analysis and collected metadata:
1. Write a concise header summarizing the change, using a suitable type prefix by default for new messages. Omit the prefix if the change is difficult to classify. Accept existing unprefixed titles as valid.
2. Explain the problem, solution, and any relevant limitations
3. Include relevant, verified GitHub issue references when applicable
4. Mark breaking changes and describe their impact and migration when applicable.
5. Include authorized DCO sign-offs and preserve existing authorship metadata. Example names and emails are placeholders, not identities to use in a real commit.
6. All commit messages should be in English.

### Step 7: User Review Loop

Present the generated commit message to the user. If in amend mode, show a before/after comparison:

```
=== BEFORE ===
<old commit message>

=== AFTER ===
<new commit message>
```
Ask the user to review using `AskUserQuestion`:

- If user approves: proceed to Step 8
- If user requests changes: use `AskUserQuestion` to ask which fields need updating (header, body, issue reference), then modify accordingly and present again. **Repeat until the user approves.**

**Repeat until the user approves.**

### Step 8: Apply Commit Message

- **Create mode**: Run `git commit -s -m "<message>"` with the authorized contributor identity (ensure changes are staged first).
- **Amend mode**: Run `git commit --amend -m "<message>"` with the preserved sign-offs. Use `-s` to add the current contributor's sign-off only when authorized and needed.
- Inspect the resulting message to verify the required sign-off is present and that no placeholder identity or duplicate sign-off was introduced.

Important: Preserve existing `Co-authored-by` and `Signed-off-by` lines.

### Step 9: Code Review

After the commit is applied, perform an automated code review **before** pushing. Read the review rules from `.agents/review_rules.yaml` in the project root, then review the diff (`git diff HEAD~1`).

**Review process:**

1. Read `.agents/review_rules.yaml` for the full checklist and rules.
2. Run `git diff HEAD~1` to get the committed changes.
3. Check the message against CONTRIBUTING.md: issue references when applicable and required DCO sign-offs, preserved authorship, and breaking-change impact and migration details when applicable. Apply the relevant rules against the diff:
   - Naming & Consistency
   - Error Handling
   - Memory Safety
   - Platform Compatibility
   - Thread Safety
   - Device Code
   - API & Interface
   - Code Style
4. Present findings to the user, categorized as:
   - **Critical**: Must fix before push (hard rejects per the rules)
   - **Warning**: Recommended to fix
   - **Info**: Minor suggestions or observations
5. If critical issues are found, help the user fix them (amend the commit) and re-review.
6. If no critical issues, proceed to the push decision.

### Step 10: Push Decision

Before pushing, identify the actual target repository and branch from the PR or confirmed task context, and fetch that branch. Do not assume `origin` or `main`. Use `git merge-base --is-ancestor <fetched-target-tip> HEAD` to check whether HEAD includes the latest target commit; if it does not, report the missing target commits with `git rev-list --count HEAD..<fetched-target-tip>` and recommend updating or rebasing. This is an advisory check: being behind alone does not block committing or pushing. If the target or its latest state cannot be verified, say so. Inclusion of the target tip does not prove a rebase occurred. Do not automatically rewrite history; conflicts and relevant integration issues must be addressed before merging.

After successful commit/amend and code review passes, ask the user:

"Would you like to push this branch to the remote for a GitHub PR?"

- If yes: verify the actual remote and working branch, then push to that branch and use a PR for integration. Do not push directly to protected development or release branches. When updating an authorized amended commit already published on the PR branch, verify the remote tip and use an explicit `--force-with-lease`.
- If no: do nothing, inform the user they can push manually later

## Important Notes

- Preserve line breaks and quote commit messages safely so shell characters are treated as literal text.
- If the user's working directory has both staged and unstaged changes, warn them and ask which files to include.
- The commit message body should be in English to match team conventions.
