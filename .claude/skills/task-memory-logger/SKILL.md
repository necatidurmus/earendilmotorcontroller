---
name: task-memory-logger
description: Log task changes to memory-bank after completing work. Short, honest, append-only daily logs.
---

# task-memory-logger

Log completed task summaries into the project memory bank.

## Purpose

After finishing a task, append a short, structured entry to the daily
log file. This gives future agents (and the user) a running history of
what changed and why — without re-reading diffs or conversation context.

## When to invoke

- After completing a coding task that modified files.
- After a build/verify step.
- When the user explicitly asks to log the task.
- Do NOT invoke for pure Q&A with no repo changes (see "No changes" rule).

## Memory bank location

Search for an existing memory bank directory in this order:

1. `memory-bank/`
2. `memory_bank/`
3. `.memory-bank/`
4. `docs/memory-bank/`
5. `docs/memory_bank/`

If found → create `task-logs/` inside it.

If NOT found → create `memory-bank/task-logs/` at the project root.

## Log file naming

Daily files: `memory-bank/task-logs/YYYY-MM-DD.md`

Use the current date. If the file already exists, append to it.

## Log entry format

```md
## HH:MM — <short task title>

**User request:** <one-line summary>

**Changed files:**
- `<file path>` — <short reason>

**Summary:**
- <change 1>
- <change 2>
- <change 3>

**Checks:**
- Build: <passed / failed / not run>
- Tests: <passed / failed / not run>
- Other: <git diff --check / lint / manual review / not run>

**Notes / risks:**
- <remaining issue or "None noted">
```

## How to gather info

1. Run `git status --short` to see changed files.
2. Run `git diff --stat` for a line-count summary.
3. Run `git diff --name-only` if needed for untracked files.
4. Read the conversation context for what the user asked and what was done.
5. Check if build/test commands were run and what the result was.

## Rules (non-negotiable)

- **Append only.** Never delete, overwrite, or rewrite old entries.
- **No duplicates.** One entry per task. If you already logged this task, skip.
- **Max 15–25 lines per entry.** Keep it short and technical.
- **No secrets.** Never log tokens, API keys, passwords, `.env` contents,
  or long terminal output.
- **No large diffs.** Summarize changes; do not paste code blocks.
- **Honest checks.** If build/test was not run, write "not run" — do not
  fabricate a pass result.
- **H7 untouched.** If H7 files were not changed, note "H7 untouched".
- **No code changes.** This skill does NOT modify source code. It only
  writes to `memory-bank/task-logs/`.
- **No structure damage.** Do not move, rename, or delete existing
  memory bank files.
- **Respect user opt-out.** If the user says "don't log" or "log yazma",
  skip the entry for this task.
- **Q&A only.** If the task was a question with no repo changes, write a
  brief "No code changes — Q&A only" entry (optional, can skip entirely).

## Example entry

```md
## 14:32 — Fix UART DMA RX overflow handling

**User request:** Fix RX overflow fault not latching during motor run

**Changed files:**
- `App/Src/motion_control.c` — add RX overflow check before motor drive
- `App/Inc/motion_control.h` — no change needed

**Summary:**
- Added `UartProtocol_GetRxDropCount()` check in `MotionControl_Service()`
- Raise `FAULT_UART_RX_OVERFLOW` and stop motor if overflow during active run

**Checks:**
- Build: passed (`pio run -d f411-motor-cube`)
- Tests: not run
- Other: manual review of fault path

**Notes / risks:**
- Not hardware-verified. Needs scope test on real F411.
```
