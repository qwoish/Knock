# Test fixtures

`state-0.7.0.json` was written by the store at commit 72f4709, the source of the
0.7.0 build, through its own operations (`generate_state_0_7_0.cpp`, built against
that commit's `src/store.cpp`). It holds sample records only. Pending requests and
the Trash entry were then given far-future dates so the file loads the same way
for decades. `tests/store_tests.cpp` checks that it opens unchanged and loses nothing.
