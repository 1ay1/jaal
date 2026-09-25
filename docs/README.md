# jaal docs

| doc | read it when |
|---|---|
| [reference.md](reference.md) | **looking something up**: every public type, the shape you write, and what it refuses. Start here if you're using jaal |
| [scope.md](scope.md) | deciding if jaal fits: what it offers, what it deliberately doesn't, what's planned |
| [hosts.md](hosts.md) | writing a host: a terminal, a GUI binding, a server. Worked example, checklist |
| [decisions.md](decisions.md) | you want the *why*: every major design choice, the alternatives, and the cost |
| [design.md](design.md) | you're working on jaal: the full technical design, layer by layer |
| [concurrency.md](concurrency.md) | anything touching threads: the safety model, built up from maya's real code |

The top-level [README](../README.md) has a quick tour and build steps.

Every code sample in `reference.md` is compiled by
`tests/docs/reference_examples.cpp`, so a sample that drifts from the API
breaks the build rather than a reader's afternoon.
