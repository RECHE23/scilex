# Threads

A lexer is immutable once built: its DFAs are built in the constructor, and every `tokenize` call and
every `scan` range keeps its own mode stack and walk memos. One `const` lexer can therefore be shared by
any number of threads, each lexing its own text; the test suite checks this under ThreadSanitizer with
eight threads over four grammars, the hybrid ones included.

An iterator from `scan` is a cursor: drive each from one thread.

In Python, `tokenize` releases the GIL around the scan of inputs of 4 KiB and more, so threads lexing
large inputs run in parallel; `scan` holds the GIL for each one-token step.

The rules left on the per-rule path call `real::regex`. Up to REAL 2026.9.7 its lazy-DFA caches were
shared by all threads behind one lock per regex, so threads scanning with such a rule queued on it;
REAL's `main` gives each thread its own caches, which ships with its next release.
