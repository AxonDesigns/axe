#!/usr/bin/env python3
"""Records the expected layout of every fixture with the pinned headless Chrome.

Each fixture (fixtures/<name>.html) is a fragment: the children of a surface, written with
<div> for nodes and <p> for text leaves, styled inline with the properties Axe supports.
Its first line is a comment giving the surface size: <!-- surface 400x300 -->.

The page wraps it in a prelude that matches Axe's model (docs/layout.md): every div is a
flex container, position: relative (each node is a containing block), border-box sized;
every p is a text leaf (a block holding only text) in the test font at line-height 1.25,
overflow-wrap: anywhere, white-space: pre-wrap. The surface is a column flex container
of the given size. A script waits for the font, then writes each element's border box,
relative to its parent's, in document order, into the DOM, which --dump-dom prints.

    python3 -I tools/layout-fixtures/fetch_chrome.py
    python3 -I tools/layout-fixtures/record.py [--scale 1|1.25|1.5] [name ...]

Every fixture is recorded at device scale factors 1, 1.25 and 1.5 (or the one given), into
expected/<name>.json, expected-125/<name>.json and expected-150/<name>.json; commit them
with the fixtures.
"""
import html
import json
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import fetch_chrome  # noqa: E402

FONT = ROOT / "tests" / "fonts" / "NotoSans-Regular-Static.ttf"

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><style>
@font-face {{ font-family: "Axe Test"; src: url("{font}"); }}
html, body {{ margin: 0; padding: 0; }}
#surface {{ display: flex; flex-direction: column; position: relative; width: {w}px;
           height: {h}px; box-sizing: border-box; }}
#surface div {{ display: flex; position: relative; box-sizing: border-box;
               border-style: solid; border-width: 0; }}
#surface p {{ display: block; position: relative; box-sizing: border-box; margin: 0;
             border-style: solid; border-width: 0; font-family: "Axe Test"; font-size: 16px;
             line-height: 1.25; overflow-wrap: anywhere; white-space: pre-wrap;
             font-kerning: normal; font-synthesis: none; }}
</style></head><body><div id="surface">{body}</div>
<pre id="axe-result"></pre>
<script>
document.fonts.ready.then(() => {{
  const out = [];
  const visit = (el) => {{
    for (const child of el.children) {{
      if (child.id === "axe-result") continue;
      const r = child.getBoundingClientRect(), p = child.parentElement.getBoundingClientRect();
      out.push([r.left - p.left, r.top - p.top, r.width, r.height]);
      visit(child);
    }}
  }};
  visit(document.getElementById("surface"));
  document.getElementById("axe-result").textContent = JSON.stringify(out);
}});
</script></body></html>
"""

SURFACE = re.compile(r"<!--\s*surface\s+(\d+)x(\d+)\s*-->")


SCALES = {1.0: "expected", 1.25: "expected-125", 1.5: "expected-150"}


def record(chrome: pathlib.Path, fixture: pathlib.Path, scale: float) -> dict:
    source = fixture.read_text(encoding="utf-8")
    m = SURFACE.search(source)
    if not m:
        raise SystemExit(f"{fixture.name}: missing <!-- surface WxH --> comment")
    w, h = int(m.group(1)), int(m.group(2))
    page = PAGE.format(font=FONT.as_uri(), w=w, h=h, body=source)
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "page.html"
        path.write_text(page, encoding="utf-8")
        out = subprocess.run(
            [str(chrome), "--headless", "--no-sandbox", "--disable-gpu", "--dump-dom",
             "--virtual-time-budget=10000", "--font-render-hinting=none",
             f"--force-device-scale-factor={scale}", "--allow-file-access-from-files",
             f"--user-data-dir={tmp}/profile", path.as_uri()],
            capture_output=True, text=True, timeout=120, check=True).stdout
    m = re.search(r'<pre id="axe-result">(.*?)</pre>', out, re.S)
    if not m or not m.group(1).strip():
        raise SystemExit(f"{fixture.name}: no result in Chrome's output")
    boxes = json.loads(html.unescape(m.group(1)))
    return {"chrome": fetch_chrome.VERSION, "scale": scale, "surface": [w, h], "boxes": boxes}


def main() -> int:
    chrome = fetch_chrome.binary()
    if not chrome.exists():
        print("run fetch_chrome.py first", file=sys.stderr)
        return 1
    args = sys.argv[1:]
    scales = list(SCALES)
    if args[:1] == ["--scale"]:
        scales = [float(args[1])]
        args = args[2:]
    fixtures = sorted((HERE / "fixtures").glob("*.html"))
    if args:
        fixtures = [f for f in fixtures if f.stem in args]
    for scale in scales:
        out = HERE / SCALES[scale]
        out.mkdir(exist_ok=True)
        for f in fixtures:
            result = record(chrome, f, scale)
            if scale == 1.0:
                del result["scale"]  # as recorded before scales were added
            (out / f"{f.stem}.json").write_text(json.dumps(result) + "\n")
            print(f"{f.stem} @{scale}: {len(result['boxes'])} boxes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
