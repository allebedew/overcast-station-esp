# Project rules

- **Do not start implementing until the user confirms.** Investigate, then
  describe what you intend to change — files, approach, anything ambiguous —
  and wait for an explicit go-ahead before the first edit.
- Code comments, commit messages and in-repo docs: **English**.
  Conversation with the user: **Russian**.
  (Comments written before 2026-07-17 are in Russian — translate them
  opportunistically when touching that code, no mass rewrite.)
- **Answer briefly.** No preamble, no restating the request, no summary of
  what was just shown. Report the result in a line or two; expand only when
  asked.
- **Comment sparingly.** Only what cannot be read off the code: a non-obvious
  *why*, a magic constant, a hardware quirk, a concurrency rule, a display
  layout the arithmetic depends on. No restating what the next line does, no
  essays, no comment on a self-explanatory name. One or two lines each, three
  at the outside; prefer a clearer name over a comment explaining an unclear
  one. Write it terse the first time — the justification for a decision,
  the alternatives weighed, the accuracy budget belong in the conversation,
  not in the source.
- README.md is the context-recovery document. When a change touches anything
  described there (features, LED colors, Wi-Fi logic, module table, HTTP API
  table, partition layout, build/flash commands), update README in the same
  session. Keep it short and factual — tables and short bullets over prose,
  no narration of design reasoning that the code already carries. Editing an
  existing section means rewriting it, not appending to it. A new feature is
  usually a row in a table or a clause in an existing bullet, not a paragraph
  of its own.
- A value derived from a single device's own readings and shown on that
  device's card belongs in that device's module, not in whoever displays it.
- **Do not build after a change** unless there is a strong reason to — a build
  takes minutes and the user usually builds and flashes from their own
  terminal. Finish the edits and say what was changed instead. Strong reasons:
  the user asked for a build, the change touches CMake / Kconfig / partitions /
  `sdkconfig.defaults`, or the edit is large enough that a compile error is a
  real possibility rather than a formality.
- When a build *is* warranted, always use the wrapper from the project root —
  it sources the IDF environment itself, which the fish login shell lacks:

  ```
  ./build.sh 2>&1 | tail -30
  ```

  Target is `esp32c6` and is already fixed in `sdkconfig`; do not pass
  `set-target`, it would wipe the config. Same form for other actions —
  `./build.sh flash monitor`, `./build.sh size`, `./build.sh fullclean`.
  `IDF_PATH` defaults to `~/esp/esp-idf`; set it to use another checkout.
