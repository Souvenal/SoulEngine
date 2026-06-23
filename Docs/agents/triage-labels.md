# Triage Labels

This repo uses GitHub's default labels. For a personal project the standard three are sufficient.

| Label           | Meaning                             |
| --------------- | ----------------------------------- |
| `bug`           | Something isn't working             |
| `documentation` | Improvements or additions to docs   |
| `enhancement`   | New feature or request              |

When a skill mentions a triage role (e.g. "needs-triage", "ready-for-agent"), map it to the closest label above. If none fits, skip the label — don't create new ones.

- `needs-triage` / `needs-info` -> no label (default state)
- `ready-for-agent` / `ready-for-human` -> `enhancement`
- bug reports -> `bug`
- docs issues -> `documentation`
- `wontfix` -> close without label
