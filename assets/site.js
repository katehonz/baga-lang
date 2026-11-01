(function () {
  'use strict';

  var isBg = (document.documentElement.lang || 'bg').indexOf('bg') === 0;
  var L = isBg
    ? { copy: 'копирай', copied: 'копирано', failed: 'неуспешно' }
    : { copy: 'copy', copied: 'copied', failed: 'failed' };

  var KEYWORDS = new Set([
    'fn', 'let', 'mut', 'if', 'else', 'while', 'for', 'in', 'return', 'struct',
    'enum', 'match', 'spec', 'import', 'extern', 'pub', 'const', 'go', 'join',
    'chan', 'select', 'case', 'default', 'catch', 'true', 'false', 'defer',
    'drop', 'trait', 'impl', 'type', 'as', 'use', 'mod', 'and', 'or', 'not',
    'world', 'requires', 'ensures', 'decreases', 'invariant', 'input', 'output',
    'violates', 'nil'
  ]);

  var TYPES = new Set([
    'i64', 'i32', 'u64', 'u8', 'f64', 'bool', 'str', 'bytes', 'void', 'Vec',
    'Map', 'Option', 'Result', 'Any', 'World'
  ]);

  var WORD_RE = /[\p{L}\p{N}_]/u;

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function classify(word, next) {
    if (KEYWORDS.has(word)) return 'kw';
    if (TYPES.has(word)) return 'type';
    if (/^(world|wvec|wmap|wcache|wrec|wstr|wbytes)_/.test(word)) return 'builtin';
    if (/^(vec|map|str|bytes|fmt|json|os|math|mem|par|chan)_/.test(word)) return 'builtin';
    if (next === '(') return 'fn';
    return '';
  }

  function highlight(code) {
    var out = '';
    var i = 0;
    var n = code.length;
    while (i < n) {
      var c = code[i];
      var c2 = code[i + 1];
      if (c === '/' && c2 === '/') {
        var j = code.indexOf('\n', i);
        if (j < 0) j = n;
        out += '<span class="tok-com">' + esc(code.slice(i, j)) + '</span>';
        i = j;
        continue;
      }
      if (c === '"') {
        var s = i + 1;
        while (s < n) {
          if (code[s] === '\\') { s += 2; continue; }
          if (code[s] === '"') { s += 1; break; }
          s += 1;
        }
        out += '<span class="tok-str">' + esc(code.slice(i, s)) + '</span>';
        i = s;
        continue;
      }
      if (c === '!' && /[A-Za-z]/.test(code[i + 1] || '')) {
        var e = i + 1;
        while (e < n && /[A-Za-z0-9_]/.test(code[e])) e += 1;
        out += '<span class="tok-eff">' + esc(code.slice(i, e)) + '</span>';
        i = e;
        continue;
      }
      if (/[0-9]/.test(c)) {
        var d = i;
        while (d < n && /[0-9_]/.test(code[d])) d += 1;
        out += '<span class="tok-num">' + esc(code.slice(i, d)) + '</span>';
        i = d;
        continue;
      }
      if (WORD_RE.test(c)) {
        var w = i;
        while (w < n && WORD_RE.test(code[w])) w += 1;
        var word = code.slice(i, w);
        var k = w;
        while (k < n && (code[k] === ' ' || code[k] === '\t')) k += 1;
        var cls = classify(word, code[k]);
        out += cls ? '<span class="tok-' + cls + '">' + esc(word) + '</span>' : esc(word);
        i = w;
        continue;
      }
      out += esc(c);
      i += 1;
    }
    return out;
  }

  function highlightAll() {
    document.querySelectorAll('pre.code[data-hl], pre > code.language-baga').forEach(function (el) {
      var target = el.tagName === 'CODE' ? el : el;
      if (target.dataset.hlDone) return;
      var raw = target.textContent;
      target.dataset.raw = raw;
      target.innerHTML = highlight(raw);
      target.dataset.hlDone = '1';
    });
  }

  function legacyCopy(text) {
    return new Promise(function (resolve, reject) {
      var ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand('copy') ? resolve() : reject(new Error('copy'));
      } catch (err) {
        reject(err);
      } finally {
        document.body.removeChild(ta);
      }
    });
  }

  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      return navigator.clipboard.writeText(text).catch(function () {
        return legacyCopy(text);
      });
    }
    return legacyCopy(text);
  }

  document.querySelectorAll('.copy-btn').forEach(function (btn) {
    btn.textContent = L.copy;
    btn.addEventListener('click', function () {
      var shell = btn.closest('.code-shell, .install, .panel');
      var src = shell ? shell.querySelector('pre.code, pre, .repl') : null;
      if (!src) return;
      var raw = src.dataset.raw || src.textContent;
      copyText(raw).then(function () {
        btn.textContent = L.copied;
        setTimeout(function () { btn.textContent = L.copy; }, 1600);
      }).catch(function () {
        btn.textContent = L.failed;
        setTimeout(function () { btn.textContent = L.copy; }, 1600);
      });
    });
  });

  document.querySelectorAll('[data-tabs]').forEach(function (group) {
    var tabs = Array.prototype.slice.call(group.querySelectorAll('[role="tab"]'));
    var panels = tabs.map(function (tab) {
      return document.getElementById(tab.getAttribute('aria-controls'));
    });

    function select(index) {
      tabs.forEach(function (tab, i) {
        var active = i === index;
        tab.setAttribute('aria-selected', active ? 'true' : 'false');
        tab.tabIndex = active ? 0 : -1;
        if (panels[i]) panels[i].hidden = !active;
      });
    }

    tabs.forEach(function (tab, i) {
      tab.addEventListener('click', function () { select(i); });
      tab.addEventListener('keydown', function (ev) {
        if (ev.key !== 'ArrowRight' && ev.key !== 'ArrowLeft') return;
        ev.preventDefault();
        var next = ev.key === 'ArrowRight' ? (i + 1) % tabs.length : (i - 1 + tabs.length) % tabs.length;
        tabs[next].focus();
        select(next);
      });
    });
    select(0);
  });

  var nav = document.querySelector('.nav');
  var burger = document.getElementById('nav-toggle');
  if (nav && burger) {
    burger.addEventListener('click', function () {
      var open = nav.classList.toggle('open');
      burger.setAttribute('aria-expanded', open ? 'true' : 'false');
    });
    nav.querySelectorAll('.nav-links a').forEach(function (a) {
      a.addEventListener('click', function () {
        nav.classList.remove('open');
        burger.setAttribute('aria-expanded', 'false');
      });
    });
    document.addEventListener('keydown', function (ev) {
      if (ev.key === 'Escape' && nav.classList.contains('open')) {
        nav.classList.remove('open');
        burger.setAttribute('aria-expanded', 'false');
        burger.focus();
      }
    });
  }

  if ('IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add('in');
          io.unobserve(entry.target);
        }
      });
    }, { threshold: 0.1, rootMargin: '0px 0px -40px 0px' });
    document.querySelectorAll('.reveal').forEach(function (el) { io.observe(el); });
  } else {
    document.querySelectorAll('.reveal').forEach(function (el) { el.classList.add('in'); });
  }

  var hero = document.querySelector('.hero');
  if (hero && window.matchMedia('(pointer: fine)').matches) {
    hero.addEventListener('mousemove', function (ev) {
      var r = hero.getBoundingClientRect();
      hero.style.setProperty('--mx', (ev.clientX - r.left) + 'px');
      hero.style.setProperty('--my', (ev.clientY - r.top) + 'px');
    });
  }

  var filterButtons = document.querySelectorAll('[data-filter]');
  if (filterButtons.length) {
    var posts = Array.prototype.slice.call(document.querySelectorAll('.blog-list [data-lang]'));
    var count = document.querySelector('.blog-count');
    filterButtons.forEach(function (btn) {
      btn.addEventListener('click', function () {
        var kind = btn.getAttribute('data-filter');
        filterButtons.forEach(function (b) {
          b.setAttribute('aria-pressed', b === btn ? 'true' : 'false');
        });
        var shown = 0;
        posts.forEach(function (post) {
          var show = kind === 'all' || post.getAttribute('data-lang') === kind;
          post.hidden = !show;
          if (show) {
            post.classList.add('in');
            shown += 1;
          }
        });
        if (count) count.textContent = shown + (isBg ? ' публикации' : ' posts');
      });
    });
  }

  var year = document.getElementById('year');
  if (year) year.textContent = String(new Date().getFullYear());

  highlightAll();
})();
