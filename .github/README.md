# GitHub contribution checks

`ISSUE_TEMPLATE/` adapts the NCCL issue, question, and feature-request forms
to PCCL environments and adds a task form with goals and acceptance criteria.
The task form assigns the organization's `Task` issue type. Other forms leave
the issue type unset for maintainers to assess. `pull_request_template.md`
follows the NCCL template. Create the `triage`, `question`, and `enhancement`
repository labels before enabling the issue forms on the default branch.

## Pre-check

`workflows/pr-test-ppu.yml` includes pre-check jobs for pull requests targeting `main`:

- Secret Scan calls the T-Head organization's TruffleHog workflow.
- Commit Format and DCO checks non-merge contribution commits for a nonempty
  title of at most 50 characters, a blank second line, and a Signed-off-by line.
  It checks the line's format, not the contributor's legal authority.
- Issue references are optional. Imperative mood and the quality of the body
  require review rather than a keyword-based rejection.

The commit-format job uses read-only permissions and a GitHub-hosted runner. It
checks Git metadata without executing PR code or requiring repository secrets.
The existing secret scan runs once. Both checks must pass before AI review and
PPU test preparation proceed. On push and manual runs, these PR-only checks are
skipped and the existing hardware CI flow continues.

After publishing, validate the workflow on a representative PR and select the
reported checks as required checks in the target branch ruleset. Workflow files
alone do not prevent merging. The existing workflow's branch filters are unchanged.

## AI review

`.github/copilot-instructions.md` provides the GitHub Copilot entry point and
references `.agents/review_rules.yaml`, the shared detailed checklist used by
local reviewers. Keep the detailed rules in one place.

Enable automatic Copilot review and review-on-push for the intended branches in
GitHub repository rulesets. Confirm custom instructions are enabled and inspect a
real review to verify that the instructions were used. These settings are separate
from the pre-check workflow and depend on Copilot availability for the repository.

The presence of a Copilot review is not proof that the latest commit was reviewed
or that findings were resolved. Any review gate must check the current head SHA
and apply the team's resolution policy. Human approval and required CI checks
remain separate controls.
