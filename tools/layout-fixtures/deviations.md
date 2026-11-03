# Known deviations from Chrome

Fixtures listed here are expected to fail `test_layout_conformance`; `gen.py` reads this
list, and the test fails if a listed fixture passes, so the list can't go stale. Each entry
is a line `- \`fixture_name\`: why Axe differs`.

None at the moment: all 209 fixtures match Chrome 156.0.8078.80 within the tolerances in
`docs/layout.md`.
