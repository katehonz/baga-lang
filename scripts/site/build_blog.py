#!/usr/bin/env python3
"""Generate static blog pages for baga-lang.top.

Usage (from the repo root):
    python3 scripts/site/build_blog.py

Reads blog/*.md and writes blog/index.html + blog/<slug>.html. The generated
pages are committed; the GitHub Pages workflow uploads the repo as-is, so there
is no build step in CI. Requires the `markdown` Python package.
"""

import html
import posixpath
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

try:
    import markdown
except ImportError:
    sys.exit("build_blog.py: python3-markdown is required (pip install markdown)")

ROOT = Path(__file__).resolve().parents[2]
BLOG = ROOT / "blog"
SITE = "https://baga-lang.top"
REPO = "https://github.com/katehonz/baga-lang"
BLOB = REPO + "/blob/main/"

MONTHS_EN = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

I18N = {
    "bg": {
        "home": "Начало",
        "idea": "Идея",
        "worlds": "Светове",
        "verify": "Верификатор",
        "blog": "Блог",
        "all": "Всички",
        "bulgarian": "Български",
        "posts": "публикации",
        "read": "Прочети",
        "minutes": "мин",
        "toc": "Съдържание",
        "back": "Всички публикации",
        "source": "Източник в GitHub",
        "skip": "Към съдържанието",
        "blog_kicker": "Блог",
        "blog_title": "Бележки от строежа на езика.",
        "blog_lead": "Светове, верификатор, памет и сторидж — какво е проверено, какво е измерено и какво още не е слепено заедно.",
        "home_path": "/",
        "article": "публикация",
        "footer": "Системен език с български корени. Памет с протокол, ефекти в типа, доказателства с контрапримери.",
        "f_lang": "Език",
        "f_code": "Код",
        "f_more": "Още",
    },
    "en": {
        "home": "Home",
        "idea": "Idea",
        "worlds": "Worlds",
        "verify": "Verifier",
        "blog": "Blog",
        "all": "All",
        "bulgarian": "Bulgarian",
        "posts": "posts",
        "read": "Read",
        "minutes": "min",
        "toc": "Contents",
        "back": "All posts",
        "source": "Source on GitHub",
        "skip": "Skip to content",
        "blog_kicker": "Blog",
        "blog_title": "Notes from building a language.",
        "blog_lead": "Worlds, the verifier, memory and storage — what is proven, what is measured, and what has not been glued together yet.",
        "home_path": "/en/",
        "article": "post",
        "footer": "A systems language with Bulgarian roots. Memory with a protocol, effects in the type, proofs with counterexamples.",
        "f_lang": "Language",
        "f_code": "Code",
        "f_more": "More",
    },
}

LOGO = """<svg class="mark" viewBox="0 0 40 40" aria-hidden="true">
        <rect x="1" y="1" width="38" height="38" rx="10" fill="#0d1014" stroke="#e9b44c" stroke-opacity=".55" stroke-width="1.4"/>
        <rect x="8" y="8.5" width="24" height="7" rx="2.6" fill="#e9b44c"/>
        <rect x="8" y="19" width="24" height="6" rx="2.2" fill="#e9b44c" opacity=".42"/>
        <rect x="8" y="28" width="15" height="4.5" rx="2" fill="#e9b44c" opacity=".18"/>
        <circle cx="31.5" cy="11.5" r="2.5" fill="#6fd6c6"/>
      </svg>"""

GITHUB_ICON = """<svg viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27s1.36.09 2 .27c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z"/></svg>"""


def esc(value):
    return html.escape(value, quote=True)


def fmt_date(iso, lang):
    try:
        d = datetime.strptime(iso, "%Y-%m-%d")
    except ValueError:
        return iso
    if lang == "bg":
        return f"{d.day:02d}.{d.month:02d}.{d.year}"
    return f"{MONTHS_EN[d.month - 1]} {d.day}, {d.year}"


def strip_md(value):
    value = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", value)
    value = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", value)
    value = value.replace("⚔️", "").replace("⚔", "").replace("🐆", "")
    value = re.sub(r"[*_`>#]+", "", value)
    value = re.sub(r"\s+", " ", value)
    value = re.sub(r"\(\s*\)", "", value)
    return value.strip()


def excerpt_of(text, title):
    lines = text.splitlines()
    blocks = re.split(r"\n\s*\n", "\n".join(lines))
    for block in blocks:
        block = block.strip()
        if not block:
            continue
        first = block.lstrip()
        if first.startswith(("#", "```", "|", ">", "- ", "* ", "+ ", "    ")):
            continue
        if re.match(r"^\d+[.)]\s", first):
            continue
        plain = strip_md(block)
        if plain:
            break
    else:
        plain = strip_md(text)
    plain = plain.replace(title, "").strip()
    plain = re.sub(r"\s+", " ", plain)
    if len(plain) > 230:
        cut = plain[:230].rsplit(" ", 1)[0]
        plain = cut.rstrip(".,;:—-") + "…"
    return plain


def git_date(path):
    rel = path.relative_to(ROOT).as_posix()
    try:
        out = subprocess.run(
            ["git", "log", "-1", "--format=%cs", "--", rel],
            cwd=ROOT, capture_output=True, text=True, check=False,
        ).stdout.strip()
    except OSError:
        out = ""
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", out):
        return out
    return datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y-%m-%d")


def rewrite_links(page):
    def repl(match):
        href = match.group(1)
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", href) or href.startswith(("/", "#", "//")):
            return match.group(0)
        path, sep, frag = href.partition("#")
        if not path:
            return match.group(0)
        norm = posixpath.normpath(posixpath.join("blog", path))
        if norm.startswith(".."):
            return match.group(0)
        url = BLOB + norm + (("#" + frag) if sep else "")
        return 'href="' + url + '"'

    return re.sub(r'href="([^"]+)"', repl, page)


def mark_baga_blocks(page):
    def repl(match):
        attrs, code = match.group(1), match.group(2)
        if "class=" in attrs:
            return match.group(0)
        text = re.sub(r"<[^>]+>", "", code)
        if re.search(r"\b(fn|let|mut|spec|struct|enum|match|world_new|wvec_\w+|wmap_\w+)\b", text) and \
           not re.search(r"^\s*(git|make|\./baga|\$ )", text, re.M):
            return '<pre><code class="language-baga">' + code + "</code></pre>"
        return match.group(0)

    return re.sub(r"<pre><code(.*?)>(.*?)</code></pre>", repl, page, flags=re.S)


def nav(lang, current):
    t = I18N[lang]
    home = t["home_path"]
    links = [
        (home + "#idea", t["idea"], "idea"),
        (home + "#worlds", t["worlds"], "worlds"),
        (home + "#verify", t["verify"], "verify"),
        ("/blog/", t["blog"], "blog"),
    ]
    items = []
    for href, label, key in links:
        cur = ' aria-current="page"' if key == current else ""
        items.append(f'<a href="{href}"{cur}>{label}</a>')
    bg_cur = ' aria-current="page"' if lang == "bg" else ""
    en_cur = ' aria-current="page"' if lang == "en" else ""
    return f"""<header class="nav">
  <div class="container nav-inner">
    <a class="brand" href="{home}" aria-label="Baga">
      {LOGO}
      Бага
    </a>
    <nav class="nav-links" aria-label="Navigation">
      {''.join(items)}
    </nav>
    <div class="nav-actions">
      <span class="lang" aria-label="Language">
        <a href="/"{bg_cur}>BG</a>
        <a href="/en/"{en_cur}>EN</a>
      </span>
      <a class="icon-link" href="{REPO}" target="_blank" rel="noopener" aria-label="GitHub">
        {GITHUB_ICON}
      </a>
      <button class="burger" id="nav-toggle" type="button" aria-expanded="false" aria-label="Menu">
        <svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" aria-hidden="true"><path d="M3 5h14M3 10h14M3 15h14"/></svg>
      </button>
    </div>
  </div>
</header>"""


def head(lang, title, desc, canonical, og_type="website"):
    return f"""<!doctype html>
<html lang="{lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}</title>
<meta name="description" content="{esc(desc)}">
<meta name="theme-color" content="#07080a">
<link rel="canonical" href="{canonical}">
<link rel="icon" type="image/svg+xml" href="/assets/favicon.svg">
<meta property="og:type" content="{og_type}">
<meta property="og:title" content="{esc(title)}">
<meta property="og:description" content="{esc(desc)}">
<meta property="og:url" content="{canonical}">
<meta property="og:image" content="{SITE}/assets/og.png">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta name="twitter:card" content="summary_large_image">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Manrope:wght@400;500;600;700;800&family=Unbounded:wght@400;500;600&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<link rel="stylesheet" href="/assets/site.css">
<script>document.documentElement.classList.add('js');</script>
<noscript><style>.js .reveal{{opacity:1;transform:none}}</style></noscript>
</head>"""


def footer(lang):
    t = I18N[lang]
    home = t["home_path"]
    bg_cur = ' aria-current="page"' if lang == "bg" else ""
    en_cur = ' aria-current="page"' if lang == "en" else ""
    return f"""<footer class="footer">
  <div class="container">
    <div class="foot-grid">
      <div class="foot-brand">
        <a class="brand" href="{home}" aria-label="Baga">
          {LOGO}
          Бага
        </a>
        <p>{t['footer']}</p>
        <div class="foot-meta">MIT · v1.4.0 · C + LLVM</div>
      </div>
      <div class="foot-col">
        <h4>{t['f_lang']}</h4>
        <a href="{BLOB}docs/GETTING-STARTED-bg.md" target="_blank" rel="noopener">Getting started</a>
        <a href="{BLOB}docs/language-en.md" target="_blank" rel="noopener">Language (EN)</a>
        <a href="{BLOB}docs/language-bg.md" target="_blank" rel="noopener">Езикът (BG)</a>
        <a href="{BLOB}docs/worlds-bg.md" target="_blank" rel="noopener">Worlds</a>
      </div>
      <div class="foot-col">
        <h4>{t['f_code']}</h4>
        <a href="{REPO}" target="_blank" rel="noopener">GitHub</a>
        <a href="{BLOB}README.md" target="_blank" rel="noopener">README</a>
        <a href="{BLOB}CHANGELOG.md" target="_blank" rel="noopener">CHANGELOG</a>
        <a href="{BLOB}LICENSE" target="_blank" rel="noopener">MIT license</a>
      </div>
      <div class="foot-col">
        <h4>{t['f_more']}</h4>
        <a href="/blog/">Blog</a>
        <a href="{REPO}/issues" target="_blank" rel="noopener">Issues</a>
        <a href="{BLOB}docs/app-worlds-bg.md" target="_blank" rel="noopener">Worlds in production</a>
        <a href="{BLOB}bench/rocks/results/vs-rocksdb-scorecard.md" target="_blank" rel="noopener">Scorecard</a>
      </div>
    </div>
    <div class="foot-bottom">
      <span>© <span id="year">2026</span> Бага · MIT</span>
      <span class="right">
        <span class="lang" aria-label="Language">
          <a href="/"{bg_cur}>BG</a>
          <a href="/en/"{en_cur}>EN</a>
        </span>
      </span>
    </div>
  </div>
</footer>"""


def collect_posts():
    posts = []
    for path in sorted(BLOG.glob("*.md")):
        raw = path.read_text(encoding="utf-8")
        lines = raw.splitlines()
        title_idx = None
        for i, line in enumerate(lines):
            if line.startswith("# "):
                title_idx = i
                break
        if title_idx is None:
            title = path.stem
            title_idx = -1
        else:
            title = strip_md(lines[title_idx][2:].strip())
        body = "\n".join(lines[title_idx + 1:]) if title_idx >= 0 else raw
        md = markdown.Markdown(extensions=["fenced_code", "tables", "toc", "sane_lists"])
        content = md.convert(body)
        content = mark_baga_blocks(content)
        content = rewrite_links(content)
        toc = md.toc.strip()
        if toc:
            toc = toc.replace(
                '<div class="toc">',
                f'<div class="toc"><div class="toc-label">{I18N["bg"]["toc"]}</div>',
                1,
            )
        lang = "bg" if path.stem.endswith("-bg") else "en"
        excerpt = excerpt_of(body, title)
        words = len(re.findall(r"\S+", strip_md(body)))
        posts.append({
            "path": path,
            "slug": path.stem,
            "title": title,
            "lang": lang,
            "date": git_date(path),
            "excerpt": excerpt,
            "content": content,
            "toc": toc,
            "minutes": max(2, round(words / 220)),
        })
    posts.sort(key=lambda p: (p["date"], p["title"]), reverse=True)
    return posts


def article_html(post):
    lang = post["lang"]
    t = I18N[lang]
    toc_label = I18N[lang]["toc"]
    toc = post["toc"].replace(f'>{I18N["bg"]["toc"]}<', f">{toc_label}<", 1) if post["toc"] else ""
    aside = f'<aside>{toc}</aside>' if toc else ""
    title = post["title"]
    canonical = f"{SITE}/blog/{post['slug']}.html"
    source = f"{BLOB}blog/{post['path'].name}"
    return f"""{head(lang, title + ' — Baga', post['excerpt'], canonical, 'article')}
<body>
<a class="skip-link" href="#main">{t['skip']}</a>
{nav(lang, 'blog')}
<main id="main" class="article-wrap">
  <div class="container">
    <div class="article-head">
      <div class="kicker"><b>{t['blog_kicker']}</b> {t['article']}</div>
      <div class="meta">
        <span>{fmt_date(post['date'], lang)}</span>
        <span class="lang-badge">{lang.upper()}</span>
        <span>{post['minutes']} {t['minutes']}</span>
      </div>
      <h1>{esc(title)}</h1>
    </div>
    <div class="article-layout">
      <article class="article">
        {post['content']}
      </article>
      {aside}
    </div>
    <div class="article-nav">
      <a href="/blog/">← {t['back']}</a>
      <a href="{source}" target="_blank" rel="noopener">{t['source']} ↗</a>
    </div>
  </div>
</main>
{footer(lang)}
<script src="/assets/site.js" defer></script>
</body>
</html>
"""


def card_html(post):
    t = I18N[post["lang"]]
    return f"""<a class="post reveal" data-lang="{post['lang']}" href="{post['slug']}.html">
  <span class="meta"><span>{fmt_date(post['date'], post['lang'])}</span><span class="lang-badge">{post['lang'].upper()}</span><span>{post['minutes']} {t['minutes']}</span></span>
  <h3>{esc(post['title'])}</h3>
  <p>{esc(post['excerpt'])}</p>
  <span class="more">{t['read']} →</span>
</a>"""


def index_html(posts):
    t = I18N["bg"]
    cards = "\n".join(card_html(p) for p in posts)
    count = len(posts)
    canonical = f"{SITE}/blog/"
    return f"""{head('bg', 'Блог — Бага', t['blog_lead'], canonical)}
<body>
<a class="skip-link" href="#main">{t['skip']}</a>
{nav('bg', 'blog')}
<main id="main">
  <section class="blog-hero">
    <div class="container">
      <div class="kicker"><b>07</b> {t['blog_kicker']}</div>
      <h1>{t['blog_title']}</h1>
      <p>{t['blog_lead']}</p>
    </div>
  </section>
  <div class="container">
    <div class="blog-toolbar">
      <div class="filters" role="group" aria-label="Език">
        <button class="filter" type="button" data-filter="all" aria-pressed="true">{t['all']}</button>
        <button class="filter" type="button" data-filter="bg" aria-pressed="false">{t['bulgarian']}</button>
        <button class="filter" type="button" data-filter="en" aria-pressed="false">English</button>
      </div>
      <span class="blog-count">{count} {t['posts']}</span>
    </div>
    <div class="blog-list">
{cards}
    </div>
  </div>
</main>
{footer('bg')}
<script src="/assets/site.js" defer></script>
</body>
</html>
"""


def main():
    posts = collect_posts()
    if not posts:
        sys.exit("build_blog.py: no blog/*.md files found")
    for post in posts:
        out = BLOG / (post["slug"] + ".html")
        out.write_text(article_html(post), encoding="utf-8")
        print(f"  {out.relative_to(ROOT)}")
    out = BLOG / "index.html"
    out.write_text(index_html(posts), encoding="utf-8")
    print(f"  {out.relative_to(ROOT)}")
    print(f"build_blog.py: {len(posts)} posts")


if __name__ == "__main__":
    main()
