# Project rules

- Code comments, commit messages and in-repo docs: **English**.
  Conversation with the user: **Russian**.
  (Comments written before 2026-07-17 are in Russian — translate them
  opportunistically when touching that code, no mass rewrite.)
- **Comment sparingly.** Only what cannot be read off the code: a non-obvious
  *why*, a magic constant, a hardware quirk, a concurrency rule, a display
  layout the arithmetic depends on. No restating what the next line does, no
  essays, no comment on a self-explanatory name. One or two lines each; prefer
  a clearer name over a comment explaining an unclear one.
- README.md is the context-recovery document. When a change touches anything
  described there (features, LED colors, Wi-Fi logic, module table, HTTP API
  table, partition layout, build/flash commands), update README in the same
  session. Keep it short and factual — tables and short bullets over prose,
  no narration of design reasoning that the code already carries. Editing an
  existing section means rewriting it, not appending to it.
- **Do not build after a change** unless there is a strong reason to — a build
  takes minutes and the user usually builds and flashes from their own
  terminal. Finish the edits and say what was changed instead. Strong reasons:
  the user asked for a build, the change touches CMake / Kconfig / partitions /
  `sdkconfig.defaults`, or the edit is large enough that a compile error is a
  real possibility rather than a formality.
- When a build *is* warranted, always use exactly this command — the login
  shell is fish and does not have the IDF environment, so it is run through
  bash with `export.sh` sourced explicitly:

  ```
  bash -c 'set -e; . /Users/alex/esp/esp-idf/export.sh >/dev/null; cd /Users/alex/Desktop/Weather/station/src; idf.py build' 2>&1 | tail -30
  ```

  Target is `esp32c6` and is already fixed in `sdkconfig`; do not pass
  `set-target`, it would wipe the config. Same form for other actions —
  substitute `idf.py flash monitor`, `idf.py size`, `idf.py fullclean`.
