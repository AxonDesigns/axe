# Known deviations from Chrome

Fixtures listed here are expected to fail `test_layout_conformance`; `gen.py` reads this
list, and the test fails if a listed fixture passes, so the list can't go stale. Each entry
is a line `- \`fixture_name\`: why Axe differs`, for every scale, or
`- \`fixture_name@125\`: ...` for one (100, 125 or 150).

None at the moment: all 209 fixtures match Chrome 156.0.8078.80 within the tolerances in
`docs/layout.md`, at device scale factors 1, 1.25 and 1.5.
