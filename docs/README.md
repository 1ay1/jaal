# jaal docs

| doc | read it when |
|---|---|
| [manual.md](manual.md) | **learning jaal**: builds one app step by step, from the first program to testing it under every thread order. Start here |
| [reference.md](reference.md) | **looking something up**: every public type, the shape you write, and what it refuses |
| [hosts.md](hosts.md) | writing a host: a terminal, a GUI binding, a server. Worked example, checklist |
| [scope.md](scope.md) | deciding if jaal fits: what it offers, what it deliberately doesn't, what's planned |
| [decisions.md](decisions.md) | you want the *why*: every major design choice, the alternatives, and the cost |
| [design.md](design.md) | you're working on jaal: the full technical design, layer by layer |
| [concurrency.md](concurrency.md) | anything touching threads: the safety model and why it's shaped this way |

The top-level [README](../README.md) has a quick tour and build steps.

Every code sample in `manual.md` and `reference.md` is compiled and run by
`tests/docs/manual_examples.cpp` and `tests/docs/reference_examples.cpp`,
so a sample that drifts from the API breaks the build rather than a
reader's afternoon.
