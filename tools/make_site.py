#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the TransitFM1 site (GitHub Pages): the web installer and the released package with its licences.

  tools/make_site.py PACKAGES OUT

PACKAGES is a folder with transit-X.Y.fwsc for the first release of web/releases.json (build/release-X.Y/ of
./build.sh --release X.Y, or the assets of the GitHub release, as .github/workflows/pages.yml downloads them). The
package must have the SHA-256 the list gives: the page checks it again in the browser before it writes anything.

OUT gets index.html, transit.js, fm1ota.js, fm1pkg.js, releases.json, and firmware/ with the package, SHA256SUMS,
LICENSE, LICENSING.md, LICENSES/ (the package holds three JieLi AC79 SDK files, Apache-2.0, and the Terminus font,
OFL-1.1: their licences go with it) and an index page listing them. Older releases of the list are published too
when their packages are in PACKAGES; a missing one is skipped (only the first is required).
"""
import hashlib
import html
import json
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
WEB = SRC / "web"
PAGE_FILES = ("index.html", "transit.js", "fm1ota.js", "fm1pkg.js", "releases.json")


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    pkgs, out = Path(argv[1]), Path(argv[2])
    data = json.loads((WEB / "releases.json").read_text(encoding="utf-8"))
    rels = data["releases"]
    if not rels:
        raise SystemExit("make_site: web/releases.json lists no release")
    if out.exists():
        shutil.rmtree(out)
    (out / "firmware" / "LICENSES").mkdir(parents=True)
    for f in PAGE_FILES:
        shutil.copy(WEB / f, out / f)
    sums, rows = [], []
    for i, r in enumerate(rels):
        name = f"transit-{r['version']}.fwsc"
        p = pkgs / name
        if not p.is_file():
            if i == 0:
                raise SystemExit(f"make_site: {p} is missing (the release the page installs)")
            print(f"make_site: skipped {name} (not in {pkgs})")
            continue
        raw = p.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        if sha != r["sha256"]:
            raise SystemExit(f"make_site: {name} has sha256 {sha}, web/releases.json says {r['sha256']}")
        shutil.copy(p, out / "firmware" / name)
        sums.append(f"{sha}  {name}\n")
        rows.append(f'<tr><td><a href="{html.escape(name)}">{html.escape(name)}</a></td><td>{len(raw)} B</td>'
                    f'<td><code>{sha}</code></td></tr>')
    (out / "firmware" / "SHA256SUMS").write_text("".join(sums))
    for f in ("LICENSE", "LICENSING.md"):
        shutil.copy(SRC / f, out / "firmware" / f)
    lic = sorted((SRC / "LICENSES").glob("*.txt"))
    for f in lic:
        shutil.copy(f, out / "firmware" / "LICENSES" / f.name)
    links = "".join(f'<li><a href="LICENSES/{html.escape(f.name)}">{html.escape(f.name)}</a></li>' for f in lic)
    (out / "firmware" / "index.html").write_text(f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>TransitFM1 packages</title>
<style>body{{font:16px/1.5 system-ui,sans-serif;max-width:860px;margin:0 auto;padding:24px 16px;background:#fff;color:#15181d}}
@media (prefers-color-scheme: dark){{body{{background:#0f1216;color:#e8ebef}}a{{color:#5fb0f2}}}}
table{{border-collapse:collapse;width:100%}}td{{padding:6px 8px;border-bottom:1px solid #8884;word-break:break-all}}</style>
</head><body>
<h1>TransitFM1 packages</h1>
<p><a href="../">The installer</a> installs the first. <a href="SHA256SUMS">SHA256SUMS</a></p>
<table>{''.join(rows)}</table>
<h2>Licences</h2>
<p>TransitFM1 is GPL-3.0-only (<a href="LICENSE">LICENSE</a>, <a href="LICENSING.md">LICENSING.md</a>; source:
<a href="{html.escape(data['repo'])}">{html.escape(data['repo'])}</a>). Each package also holds three files of the JieLi
AC79 SDK (Apache-2.0) and the Terminus font (OFL-1.1):</p>
<ul>{links}</ul>
</body></html>
""", encoding="utf-8")
    print(f"site: {out}: {', '.join(PAGE_FILES)}; firmware/ {len(sums)} package(s), SHA256SUMS, LICENSE, "
          f"LICENSING.md, LICENSES/ ({len(lic)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
