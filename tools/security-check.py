#!/usr/bin/env python3
"""Woechentliche Sicherheitspruefung der Fremdbibliotheken von peek.

Prueft:
  1. C/C++-Bibliotheken (vcpkg, Stand aus vcpkg.json "builtin-baseline") gegen die NVD (CVE-Datenbank),
  2. stb_image (keine Versionsnummern): neue CVEs der letzten Wochen, PSD-Bezug wird hervorgehoben,
  3. alle mitgelinkten Rust-Pakete von resvg gegen OSV (enthaelt die RustSec-Datenbank),
  4. ob vcpkg neuere Versionen der Bibliotheken anbietet.

Ausgabe: Markdown-Bericht (Datei und GitHub-Zusammenfassung). Exitcode 0; die Datei "befund.txt" wird nur
angelegt, wenn es Handlungsbedarf gibt (bekannte Luecke in einer verwendeten Version).

Bewusst ignorierte Eintraege stehen in tools/security-ignore.txt (eine ID je Zeile, danach Begruendung).

Nur Python-Standardbibliothek. Laeuft in GitHub Actions (Netzwerk), nicht in peek selbst.
"""
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# vcpkg-Port -> CPE (Hersteller:Produkt) in der NVD. None = keine CPE vorhanden.
PORTS = {
    "sqlite3": "sqlite:sqlite",
    "libwebp": "webmproject:libwebp",
    "libavif": "aomedia:libavif",
    "dav1d": "videolan:dav1d",
    "zlib": "zlib:zlib",
    "libjpeg-turbo": "libjpeg-turbo:libjpeg-turbo",
    "libspng": "libspng_project:libspng",
    "libyuv": None,
    "stb": None,
}
NVD = "https://services.nvd.nist.gov/rest/json/cves/2.0"
RESVG_FEATURES = "text,system-fonts,memmap-fonts,raster-images"


def http_json(url, data=None, headers=None, tries=4):
    hdr = {"User-Agent": "peek-security-check"}
    hdr.update(headers or {})
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        hdr["Content-Type"] = "application/json"
    for i in range(tries):
        try:
            req = urllib.request.Request(url, data=body, headers=hdr)
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code in (403, 429, 500, 502, 503) and i + 1 < tries:
                time.sleep(15 * (i + 1))
                continue
            raise
        except urllib.error.URLError:
            if i + 1 < tries:
                time.sleep(10)
                continue
            raise


def http_text(url):
    req = urllib.request.Request(url, headers={"User-Agent": "peek-security-check"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode()


def load_ignore():
    ids = {}
    p = os.path.join(ROOT, "tools", "security-ignore.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#"):
                parts = line.split(None, 1)
                ids[parts[0]] = parts[1] if len(parts) > 1 else ""
    return ids


def baseline(commit):
    url = f"https://raw.githubusercontent.com/microsoft/vcpkg/{commit}/versions/baseline.json"
    return json.loads(http_text(url))["default"]


def nvd_wait():
    # ohne API-Schluessel erlaubt die NVD 5 Anfragen je 30 Sekunden
    time.sleep(1 if os.environ.get("NVD_API_KEY") else 7)


def nvd_headers():
    k = os.environ.get("NVD_API_KEY")
    return {"apiKey": k} if k else {}


def cve_summary(v):
    c = v["cve"]
    desc = next((d["value"] for d in c.get("descriptions", []) if d.get("lang") == "en"), "")
    score = ""
    for key in ("cvssMetricV40", "cvssMetricV31", "cvssMetricV30"):
        m = c.get("metrics", {}).get(key)
        if m:
            score = f'{m[0]["cvssData"]["baseScore"]} {m[0]["cvssData"].get("baseSeverity", "")}'.strip()
            break
    return c["id"], score, desc


def check_vcpkg(report, findings, ignore):
    manifest = json.load(open(os.path.join(ROOT, "vcpkg.json"), encoding="utf-8"))
    commit = manifest["builtin-baseline"]
    pinned = baseline(commit)
    latest = baseline("master")
    report.append(f"## C/C++-Bibliotheken (vcpkg-Stand `{commit[:10]}`)\n")
    report.append("| Bibliothek | Version | neueste in vcpkg | bekannte Lücken |")
    report.append("| --- | --- | --- | --- |")
    for port, cpe in PORTS.items():
        if port not in pinned:
            continue
        ver = pinned[port]["baseline"]
        pv = pinned[port].get("port-version", 0)
        new = latest.get(port, {}).get("baseline", ver)
        newpv = latest.get(port, {}).get("port-version", 0)
        upd = "aktuell" if (new, newpv) == (ver, pv) else f"**{new}**" + (f"#{newpv}" if newpv else "")
        if cpe is None:
            report.append(f"| {port} | {ver} | {upd} | keine CPE, siehe unten |" if port == "stb" else
                          f"| {port} | {ver} | {upd} | keine CPE in der NVD |")
            continue
        try:
            q = urllib.parse.urlencode({"virtualMatchString": f"cpe:2.3:a:{cpe}:{ver}"}) + "&noRejected"
            data = http_json(f"{NVD}?{q}", headers=nvd_headers())
            hits = [cve_summary(v) for v in data.get("vulnerabilities", [])]
        except Exception as e:  # noqa: BLE001
            report.append(f"| {port} | {ver} | {upd} | Abfrage fehlgeschlagen ({e}) |")
            nvd_wait()
            continue
        nvd_wait()
        open_hits = [h for h in hits if h[0] not in ignore]
        if not hits:
            report.append(f"| {port} | {ver} | {upd} | keine |")
            continue
        cell = ", ".join(f"{i} ({s})" if s else i for i, s, _ in open_hits) or "nur ignorierte"
        report.append(f"| {port} | {ver} | {upd} | {cell} |")
        for i, s, d in open_hits:
            findings.append(f"- **{port} {ver}: {i}** {s} – {d[:300]}")


def check_stb(report, findings, ignore, days=35):
    end = datetime.datetime.now(datetime.timezone.utc)
    start = end - datetime.timedelta(days=days)
    fmt = "%Y-%m-%dT%H:%M:%S.000"
    q = urllib.parse.urlencode({"keywordSearch": "stb_image", "pubStartDate": start.strftime(fmt),
                                "pubEndDate": end.strftime(fmt)}) + "&noRejected"
    report.append(f"\n## stb_image (peek nutzt nur den PSD-Teil)\n")
    try:
        data = http_json(f"{NVD}?{q}", headers=nvd_headers())
    except Exception as e:  # noqa: BLE001
        report.append(f"Abfrage fehlgeschlagen: {e}")
        return
    nvd_wait()
    hits = [cve_summary(v) for v in data.get("vulnerabilities", [])]
    if not hits:
        report.append(f"Keine neuen CVEs in den letzten {days} Tagen.")
        return
    for i, s, d in hits:
        psd = "psd" in d.lower()
        mark = "**betrifft PSD**" if psd else "anderes Format, nicht eingebaut"
        report.append(f"- {i} {s} – {mark}: {d[:200]}")
        if psd and i not in ignore:
            findings.append(f"- **stb_image (PSD): {i}** {s} – {d[:300]}")


def resvg_packages():
    ps1 = open(os.path.join(ROOT, "tools", "build-resvg.ps1"), encoding="utf-8").read()
    ver = re.search(r'\$Version\s*=\s*"([^"]+)"', ps1).group(1)
    tmp = tempfile.mkdtemp()
    src = os.path.join(tmp, "resvg")
    subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1", "--branch",
                    f"v{ver}", "https://github.com/linebender/resvg", src], check=True)
    out = subprocess.run(["cargo", "metadata", "--locked", "--format-version", "1", "--manifest-path",
                          os.path.join(src, "crates", "c-api", "Cargo.toml"), "--filter-platform",
                          "x86_64-pc-windows-msvc", "--no-default-features", "--features", RESVG_FEATURES],
                         check=True, capture_output=True, text=True).stdout
    meta = json.loads(out)
    # Nur Pakete, die in resvg.lib landen: normale Abhaengigkeiten ab resvg-capi. Build-Skripte und
    # Proc-Makros laufen nur beim Bauen und werden nicht mitgelinkt.
    by_id = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"] or next(i for i, p in by_id.items() if p["name"] == "resvg-capi")
    seen, todo = set(), [root]
    while todo:
        i = todo.pop()
        if i in seen:
            continue
        seen.add(i)
        for d in nodes[i].get("deps", []):
            if not any(k.get("kind") is None for k in d.get("dep_kinds", [])):
                continue
            kinds = {k for t in by_id[d["pkg"]]["targets"] for k in t["kind"]}
            if "proc-macro" in kinds:
                continue
            todo.append(d["pkg"])
    pkgs = sorted({(by_id[i]["name"], by_id[i]["version"]) for i in seen})
    return ver, pkgs


def check_rust(report, findings, ignore):
    ver, pkgs = resvg_packages()
    report.append(f"\n## Rust-Pakete (resvg {ver}, {len(pkgs)} mitgelinkte Pakete)\n")
    queries = [{"package": {"name": n, "ecosystem": "crates.io"}, "version": v} for n, v in pkgs]
    res = http_json("https://api.osv.dev/v1/querybatch", data={"queries": queries})["results"]
    any_hit = False
    for (n, v), r in zip(pkgs, res):
        for vuln in r.get("vulns", []):
            vid = vuln["id"]
            detail = http_json(f"https://api.osv.dev/v1/vulns/{vid}")
            ids = {vid, *detail.get("aliases", [])}
            summary = detail.get("summary") or detail.get("details", "")[:200]
            informational = (detail.get("database_specific") or {}).get("informational") or next(
                (a.get("database_specific", {}).get("informational") for a in detail.get("affected", [])
                 if a.get("database_specific", {}).get("informational")), None)
            line = f"- {n} {v}: {vid} – {summary}" + (f" (Hinweis: {informational})" if informational else "")
            report.append(line)
            any_hit = True
            if not (ids & set(ignore)) and not informational:
                findings.append(f"- **{n} {v}: {vid}** – {summary}")
    if not any_hit:
        report.append("Keine bekannten Lücken.")


def main():
    ignore = load_ignore()
    report = [f"# Sicherheitsprüfung peek – {datetime.date.today():%d.%m.%Y}\n"]
    findings = []
    for fn in (check_vcpkg, check_stb, check_rust):
        try:
            fn(report, findings, ignore)
        except Exception as e:  # noqa: BLE001
            report.append(f"\n**{fn.__name__} fehlgeschlagen:** {e}\n")
    if ignore:
        report.append("\n## Bewusst ignoriert\n")
        report += [f"- {k}: {v}" for k, v in ignore.items()]
    if findings:
        report.insert(1, "## Handlungsbedarf\n\nBekannte Lücken in verwendeten Versionen. Bibliothek aktualisieren "
                         "(vcpkg.json: builtin-baseline, tools/build-resvg.ps1: Version), neu bauen und ein Release "
                         "veröffentlichen – oder begründet in tools/security-ignore.txt eintragen.\n\n"
                      + "\n".join(findings) + "\n")
        open("befund.txt", "w", encoding="utf-8").write("\n".join(findings))
    else:
        report.insert(1, "Kein Handlungsbedarf: keine bekannten Lücken in den verwendeten Versionen.\n")
    text = "\n".join(report) + "\n"
    open("sicherheitsbericht.md", "w", encoding="utf-8").write(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        open(summary, "a", encoding="utf-8").write(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
