#!/usr/bin/env python3
"""VitaOS Store catalogue: every PS Vita homebrew source in one file.

    python3 catalog/build.py [out.json]        (GITHUB_TOKEN in the environment lifts GitHub's rate limit)

Sources, merged by title ID (then by name):
  * VitaHomebrewDB (drdecki.github.io/VitaHomebrewDB): the base, VitaDB's successor.
  * CBPS-DB (github.com/KuromeSan/cbps-db): adds the VPKs VitaHomebrewDB lacks.
Then every app whose download is a GitHub release is pointed at that
project's newest release with a VPK, so the Store offers the latest build.

Ports are in: the player brings their own game files, as each port's page
says. Listed but not downloadable ("blocked": the reason, shown on the page):
CBPS "DATA" packages (commercial game files), plugins (they need config.txt
edits, not an install button), the few VPKs that contain a commercial game
itself, piracy tools, adult content and test or joke uploads (BLOCK below). The output keeps
VitaHomebrewDB's JSON shape, with "source" and, for CBPS rows, "icon_url".
"""
import csv
import io
import json
import os
import re
import sys
import time
import urllib.request

VHDB = "https://drdecki.github.io/VitaHomebrewDB/apps.json"
CBPS = "https://raw.githubusercontent.com/KuromeSan/cbps-db/master/cbpsdb.csv"
UA = {"User-Agent": "VitaOS-catalog (github.com/mvizensk/VitaOS)"}

# What is listed but cannot be downloaded, and why (shown on the app's page).
BLOCK = [
    (re.compile(r"cuphead|henry ?stickmin|super mario|granny|chip and dale|alien vs predator|deltarune", re.I),
     "This VPK contains a commercial game, not just a port of its engine."),
    (re.compile(r"pkgj|\bnps\b|nopaystation|package installer|pkg enabler|decrypt|activate\.vpk|backup license", re.I),
     "A tool for installing pirated packages."),
    (re.compile(r"hentai|lolicop|porn|nsfw", re.I), "Adult content."),
    (re.compile(r"hello.?world|sample|\btests?\b|fucku|pewdiepie|stoner|cocaine|chinaissmall|socal credit|bitcoin by", re.I),
     "A test or joke upload."),
]


def blocked_reason(title):
    for rx, why in BLOCK:
        if rx.search(title):
            return why
    return ""


OWN_HOST = re.compile(r"^https://", re.I)


def get(url, token=None, accept=None):
    h = dict(UA)
    if token and "api.github.com" in url:
        h["Authorization"] = "Bearer " + token
    if accept:
        h["Accept"] = accept
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code in (403, 429) and attempt < 2:
                time.sleep(20)
                continue
            raise
        except Exception:
            if attempt < 2:
                time.sleep(3)
                continue
            raise


def norm(name):
    return re.sub(r"[^a-z0-9]", "", name.lower().replace("vita", ""))


def classify(title, tid):
    t = title.lower()
    if re.search(r"emulat|retroarch|\bemu\b|nes|snes|gba|\bds\b|n64|daedalus|mame|scumm|dosbox|psp|ppsspp|gameboy|genesis", t):
        return "5"
    if re.search(r"manager|installer|tool|ftp|shell|config|player|browser|youtube|downloader|updater|plugin|launcher|"
                 r"vitashell|backup|editor|viewer|reader|music|video|radio|calculator|clock|checker|volume|menu", t):
        return "4"
    return "1"


def readme_blurb(url):
    """The first real paragraph of a project's README, as plain text."""
    if not url or url == "None":
        return ""
    try:
        md = get(url).decode("utf-8", "replace")
    except Exception:
        return ""
    for para in re.split(r"\n\s*\n", md):
        p = para.strip()
        if not p or p.startswith(("#", "!", "<", "[!", "|", "```", "-", "*", ">")) or len(p) < 40:
            continue
        p = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", p)
        p = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", p)
        p = re.sub(r"[*_`]", "", p)
        p = re.sub(r"\s+", " ", p).strip()
        return p[:400]
    return ""


def cbps_rows(have_ids, have_names):
    out = []
    for r in csv.DictReader(io.StringIO(get(CBPS).decode("utf-8", "replace"))):
        if r.get("visible") != "True":
            continue
        tid, title, url = r["id"][:9], r["title"].strip(), r["download_url"]
        if tid in have_ids or norm(title) in have_names or not OWN_HOST.match(url):
            continue
        why = blocked_reason(title)
        if r.get("type") == "DATA":
            why = why or "Game data files: bring your own from the game you own."
        elif r.get("type") == "PLUGIN":
            why = why or "A plugin: install it by hand (it needs a line in ur0:tai/config.txt)."
        elif not url.lower().split("?")[0].endswith(".vpk"):
            continue
        added = float(r.get("time_added") or 0)
        icon_url = r["download_icon0"] if r["download_icon0"].startswith("http") and not why else ""
        out.append({
            "name": title, "titleid": tid, "author": r["credits"], "type": classify(title, tid),
            "description": readme_blurb(r.get("download_readme")), "long_description": "",
            "url": url, "version": "", "date": time.strftime("%Y-%m-%d", time.gmtime(added)) if added else "",
            "icon": ("cbps-%s.png" % tid) if icon_url else "", "icon_url": icon_url,
            "size": "0", "downloads": "0", "screenshots": "", "data": "", "requirements": "",
            "release_page": r.get("download_src") if r.get("download_src", "None") != "None" else "",
            "source": "CBPS-DB", "blocked": why,
        })
        if why:
            out[-1]["url"] = ""                         # listed, never downloadable
        have_ids.add(tid)
        have_names.add(norm(title))
    return out


GH = re.compile(r"^https://github\.com/([^/]+)/([^/]+)/releases/download/([^/]+)/", re.I)


def ver_key(tag):
    return [int(x) for x in re.findall(r"\d+", tag)][:4]


def bump_to_latest(apps, token):
    """Point GitHub-hosted apps at their newest release that carries a VPK."""
    cache, bumped = {}, 0
    for a in apps:
        m = GH.match(a.get("url", "") or "")
        if not m:
            continue
        repo = "%s/%s" % (m.group(1), m.group(2))
        if repo not in cache:
            try:
                cache[repo] = json.loads(get("https://api.github.com/repos/%s/releases/latest" % repo, token,
                                             "application/vnd.github+json"))
            except Exception:
                cache[repo] = None
        rel = cache[repo]
        if not rel or rel.get("draft") or rel.get("prerelease") or rel.get("tag_name") == m.group(3):
            continue
        vpks = [x for x in rel.get("assets", []) if x["name"].lower().endswith(".vpk")]
        if not vpks:
            continue
        old = a["url"].rsplit("/", 1)[1].lower()
        pick = next((x for x in vpks if x["name"].lower() == old), vpks[0] if len(vpks) == 1 else None)
        if not pick:
            continue
        a["url"] = pick["browser_download_url"]
        a["version"] = rel["tag_name"]
        a["size"] = str(pick.get("size") or a.get("size") or 0)
        a["date"] = (rel.get("published_at") or "")[:10] or a.get("date", "")
        a["release_page"] = rel.get("html_url", a.get("release_page", ""))
        bumped += 1
    return bumped, len(cache)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "catalog.json"
    token = os.environ.get("GITHUB_TOKEN")
    base = json.loads(get(VHDB))
    for a in base:
        a.setdefault("source", "VitaHomebrewDB")
    ids = {a.get("titleid", "") for a in base}
    names = {norm(a.get("name", "")) for a in base}
    extra = cbps_rows(ids, names)
    apps = base + extra
    bumped, repos = bump_to_latest(apps, token)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(apps, f, ensure_ascii=False, indent=1)
    print("VitaHomebrewDB %d + CBPS-DB %d (%d listed but blocked) = %d apps; %d moved to a newer GitHub release (%d repos checked)"
          % (len(base), len(extra), sum(1 for a in extra if a["blocked"]), len(apps), bumped, repos))


if __name__ == "__main__":
    main()
