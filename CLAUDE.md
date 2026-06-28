# CLAUDE.md

@AGENTS.md

## Claude-specific rules

1. **Read `AGENTS.md` first** at the start of every task.
2. **Firmware task?** Also read the relevant `docs/ai/*` file and the
   target module's header/source before editing.
3. **Plan before large refactors.** Write a short plan in the chat or
   as a comment block before touching more than 2 files.
4. **Verify after changes.** Run `pio run -d f411-motor-cube` for
   firmware, `python -m py_compile tools/*.py` for Python tools.
   If PlatformIO is not installed, report it — do not fabricate a
   build command.
5. **Log every task.** Append to `docs/ai/TASK_LOG.md` after completing
   work. Format: see `AGENTS.md` § Agent workflow.
