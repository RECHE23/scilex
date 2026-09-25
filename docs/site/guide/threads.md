# Threads

A lexer is immutable once built: its DFAs are built in the constructor, and every `tokenize` call and
every `scan` range keeps its own mode stack and walk memos. One `const` lexer can therefore be shared by
any number of threads, each lexing its own text. `make tsan` checks this under ThreadSanitizer, locally
and in CI: eight threads share each fresh lexer and start at once, over four grammars — one entirely on
its DFA, a modal one, one with layout, and a hybrid one with rules on the per-rule path — and every
thread's tokens must equal the single-threaded ones.

An iterator from `scan` is a cursor: drive each from one thread.

In Python, `tokenize` releases the GIL around the scan of inputs of 4 KiB and more, so threads lexing
large inputs run in parallel; `scan` holds the GIL for each one-token step.

The rules left on the per-rule path call `real::regex`. Up to REAL 2026.9.7 its lazy-DFA caches were
shared by all threads behind one lock per regex, so threads scanning with such a rule queued on it;
REAL's `main` gives each thread its own caches, which ships with its next release.
