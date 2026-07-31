/* SpoutPlugin Docs — app orchestration & interactivity */
(function () {
  "use strict";
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => [...(r || document).querySelectorAll(s)];
  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ============ MOUNT ============ */
  function mount() {
    document.documentElement.classList.add("js"); // arm reveal hidden-state
    buildSidebar();
    const doc = $("#doc");
    doc.innerHTML = RENDER_A();
    // inject quick-start cards into their container
    const qsHost = $("#qs-cards");
    if (qsHost) qsHost.innerHTML = renderQuickStartCards();
    doc.insertAdjacentHTML("beforeend", RENDER_B());
    renderApiCards();
    renderTroubleshooting();
    wireEverything();
    buildCmdk();
    initObservers();
    initBackground();
    initHeroPackets();
    initTweaks();
  }

  /* ============ SIDEBAR ============ */
  function buildSidebar() {
    const nav = $("#sb-nav");
    nav.innerHTML = SPOUT.nav.map(g => `
      <div class="sb-group">
        <div class="sb-group-label">${g.group}</div>
        ${g.items.map(it => `<a class="sb-link" href="#${it.id}" data-nav="${it.id}">${it.num ? `<span class="num">${it.num}</span>` : `<span class="num">${ICON.pulse(13)}</span>`}<span>${it.label}</span></a>`).join("")}
      </div>`).join("");
  }

  /* ============ API CARDS ============ */
  function renderApiCards() {
    const list = $("#api-list");
    list.innerHTML = SPOUT.api.map((n, i) => {
      const inputs = n.inputs ? `<dl class="api-grid">${n.inputs.map(p => `<dt>${C.esc(p[0])}</dt><dd>${p[1]}</dd>`).join("")}</dl>` : "";
      const outputs = n.outputs ? `<h4 class="mono" style="color:var(--success);font-size:11px;letter-spacing:.06em;margin:16px 0 6px">OUTPUTS</h4><dl class="api-grid">${n.outputs.map(p => `<dt>${C.esc(p[0])}</dt><dd>${p[1]}</dd>`).join("")}</dl>` : "";
      const mistakes = n.mistakes ? `<h4 class="mono" style="color:var(--error);font-size:11px;letter-spacing:.06em;margin:16px 0 6px">COMMON MISTAKES</h4><ul class="lead-list mistakes">${n.mistakes.map(m => `<li>${m}</li>`).join("")}</ul>` : "";
      const blob = (n.name + " " + n.sig + " " + n.purpose + " " + n.cat).toLowerCase();
      return `
      <div class="api-card" data-cat="${n.cat}" data-blob="${C.esc(blob)}">
        <div class="api-top" role="button" tabindex="0" aria-expanded="false">
          <span class="api-cat ${n.cat}">${n.cat}</span>
          <span class="api-name">${n.name}</span>
          <span class="api-ret mono">returns <b>${C.esc(n.ret)}</b></span>
          <span class="api-chev">${ICON.chevron(18)}</span>
        </div>
        <div class="api-detail"><div><div class="api-inner">
          ${C.code(n.sig)}
          <p class="api-purpose">${n.purpose}</p>
          <h4 class="mono" style="color:var(--accent);font-size:11px;letter-spacing:.06em;margin:16px 0 6px">INPUTS</h4>
          ${inputs || '<p style="font-size:13.5px;color:var(--muted)">None.</p>'}
          ${outputs}
          <dl class="api-grid"><dt>Returns</dt><dd>${n.returns}</dd><dt>When to call</dt><dd>${n.when}</dd></dl>
          ${mistakes}
        </div></div></div>
      </div>`;
    }).join("");
  }

  /* ============ TROUBLESHOOTING ============ */
  function renderTroubleshooting() {
    const list = $("#ts-list");
    list.innerHTML = SPOUT.troubleshooting.map(t => {
      const blob = (t.symptom + " " + t.fix).toLowerCase();
      return `
      <div class="ts-item" data-blob="${C.esc(blob)}">
        <div class="ts-q" role="button" tabindex="0" aria-expanded="false">
          <span class="ts-icon">${ICON.warn(18)}</span>
          <span class="ts-symptom">${t.symptom}</span>
          <span class="ts-chev">${ICON.chevron(18)}</span>
        </div>
        <div class="ts-a"><div><div class="ts-fix">${t.fix}</div></div></div>
      </div>`;
    }).join("");
  }

  /* ============ WIRE INTERACTIONS ============ */
  function wireEverything() {
    // delegated click handler
    document.addEventListener("click", (e) => {
      // copy buttons
      const cp = e.target.closest(".copy-btn, [data-copy]");
      if (cp && cp.dataset.copy != null) { doCopy(cp); return; }
      // anchor links
      const an = e.target.closest("[data-anchor]");
      if (an) { e.preventDefault(); copyAnchor(an); return; }
      // tabs
      const tb = e.target.closest(".tab-btn");
      if (tb) { switchTab(tb); return; }
      // quick-start accordion
      const qh = e.target.closest(".qs-head");
      if (qh && !e.target.closest("[data-anchor]")) { qh.closest(".qs-card").classList.toggle("open"); qh.setAttribute("aria-expanded", qh.closest(".qs-card").classList.contains("open")); return; }
      // api accordion
      const at = e.target.closest(".api-top");
      if (at) { const c = at.closest(".api-card"); c.classList.toggle("open"); at.setAttribute("aria-expanded", c.classList.contains("open")); return; }
      // troubleshooting accordion
      const tq = e.target.closest(".ts-q");
      if (tq) { const c = tq.closest(".ts-item"); c.classList.toggle("open"); tq.setAttribute("aria-expanded", c.classList.contains("open")); return; }
      // first-check chips
      const fc = e.target.closest(".ts-check");
      if (fc) { const on = fc.classList.toggle("checked"); fc.setAttribute("aria-checked", on); return; }
      // api filters
      const fch = e.target.closest(".fchip");
      if (fch) { $$("#api-filters .fchip").forEach(x => x.classList.remove("active")); fch.classList.add("active"); filterApi(); return; }
      // sender mode toggle
      const sm = e.target.closest("#sender-toggle button");
      if (sm) { setSenderMode(sm.dataset.mode); return; }
      // sender/receiver flow toggle
      const sr = e.target.closest("#sr-toggle button");
      if (sr) { setSrFlow(sr.dataset.sr); return; }
      // mobile menu
      if (e.target.closest(".mobile-menu")) { document.body.classList.toggle("nav-open"); return; }
      if (e.target.closest(".scrim")) { document.body.classList.remove("nav-open"); return; }
      // sidebar link (mobile close)
      if (e.target.closest(".sb-link")) { setTimeout(() => document.body.classList.remove("nav-open"), 120); return; }
      // back to top
      if (e.target.closest("#to-top")) { window.scrollTo({ top: 0, behavior: reduced ? "auto" : "smooth" }); return; }
      // command palette open
      if (e.target.closest(".sb-search, #cmdk-open")) { openCmdk(); return; }
    });

    // keyboard for role=button elements
    document.addEventListener("keydown", (e) => {
      if ((e.key === "Enter" || e.key === " ") && e.target.matches(".qs-head, .api-top, .ts-q, .ts-check")) {
        e.preventDefault(); e.target.click();
      }
    });

    // api search
    const asi = $("#api-search-input");
    if (asi) asi.addEventListener("input", filterApi);
    // ts search
    const tsi = $("#ts-search");
    if (tsi) tsi.addEventListener("input", filterTs);

    // black checklist toggles
    $$("#black-checklist .chk").forEach(c => c.addEventListener("click", () => c.classList.toggle("checked")));
  }

  function doCopy(btn) {
    const txt = btn.dataset.copy.replace(/&quot;/g, '"').replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">");
    navigator.clipboard?.writeText(txt).then(() => {
      const old = btn.textContent; btn.textContent = "copied"; btn.classList.add("done");
      setTimeout(() => { btn.textContent = old; btn.classList.remove("done"); }, 1400);
    });
  }
  function copyAnchor(el) {
    const sec = el.closest("[id]");
    if (!sec) return;
    const url = location.origin + location.pathname + "#" + sec.id;
    navigator.clipboard?.writeText(url);
    history.replaceState(null, "", "#" + sec.id);
    el.textContent = "✓"; setTimeout(() => el.textContent = "#", 1200);
  }
  function switchTab(btn) {
    const wrap = btn.closest(".tabs");
    $$(".tab-btn", wrap).forEach(b => b.classList.remove("active"));
    $$(".tab-panel", wrap).forEach(p => p.classList.remove("active"));
    btn.classList.add("active");
    $("#" + btn.dataset.tab, wrap).classList.add("active");
  }
  function setSenderMode(m) {
    $$("#sender-toggle button").forEach(b => b.classList.toggle("active", b.dataset.mode === m));
    $$("#sender-mode [data-mode-panel]").forEach(p => p.hidden = p.dataset.modePanel !== m);
  }
  function setSrFlow(dir) {
    $$("#sr-toggle button").forEach(b => b.classList.toggle("active", b.dataset.sr === dir));
    const host = $("#sr-flow");
    const send = [{ label: "Source", name: "Unreal Engine", sub: "viewport or render target" }, { label: "Bridge", name: "D3D11-on-D3D12", sub: "shared texture" }, { label: "Out", name: "TouchDesigner", sub: "or any Spout app" }];
    const recv = [{ label: "Source", name: "TouchDesigner", sub: "or any Spout app" }, { label: "Bridge", name: "D3D11-on-D3D12", sub: "shared texture" }, { label: "Into", name: "Unreal Engine", sub: "texture / material" }];
    host.innerHTML = C.flow(dir === "send" ? send : recv, dir === "receive");
    $(".hint-line", host.parentElement).textContent = dir === "send" ? "Sender: Unreal → external app" : "Receiver: external app → Unreal";
  }

  function filterApi() {
    const q = ($("#api-search-input").value || "").toLowerCase().trim();
    const cat = $("#api-filters .fchip.active").dataset.filter;
    let shown = 0;
    $$(".api-card").forEach(c => {
      const okCat = cat === "all" || c.dataset.cat === cat;
      const okQ = !q || c.dataset.blob.includes(q);
      const vis = okCat && okQ;
      c.classList.toggle("hidden", !vis);
      if (vis) shown++;
    });
    $("#api-empty").style.display = shown ? "none" : "block";
  }
  function filterTs() {
    const q = ($("#ts-search").value || "").toLowerCase().trim();
    let shown = 0;
    $$(".ts-item").forEach(c => {
      const vis = !q || c.dataset.blob.includes(q);
      c.classList.toggle("hidden", !vis);
      if (vis) shown++;
    });
    $("#ts-empty").style.display = shown ? "none" : "block";
  }

  /* ============ OBSERVERS: reveal, active nav, progress, cost meter ============ */
  function initObservers() {
    // --- Reveal: robust, never leaves content hidden ---
    const reveals = $$(".reveal");
    function revealInView() {
      const vh = window.innerHeight || document.documentElement.clientHeight;
      reveals.forEach(el => {
        if (el.classList.contains("in")) return;
        const r = el.getBoundingClientRect();
        if (r.top < vh * 0.94 && r.bottom > 0) el.classList.add("in");
      });
    }
    // first paint: reveal everything already on screen right now
    revealInView();
    if ("IntersectionObserver" in window) {
      const ro = new IntersectionObserver((entries) => {
        entries.forEach(en => { if (en.isIntersecting) { en.target.classList.add("in"); ro.unobserve(en.target); } });
      }, { threshold: 0.06, rootMargin: "0px 0px -6% 0px" });
      reveals.forEach(el => { if (!el.classList.contains("in")) ro.observe(el); });
    }
    // scroll fallback (covers environments where IO never fires)
    document.addEventListener("scroll", revealInView, { passive: true });
    window.addEventListener("resize", revealInView);
    // ultimate safety: nothing stays hidden
    setTimeout(() => reveals.forEach(el => el.classList.add("in")), 2500);

    // cost meter fill
    const co = new IntersectionObserver((entries) => {
      entries.forEach(en => { if (en.isIntersecting) { $$(".cost-fill", en.target).forEach(f => f.style.width = f.dataset.w + "%"); co.unobserve(en.target); } });
    }, { threshold: 0.3 });
    const cm = $("#cost-meter"); if (cm) co.observe(cm);

    // active nav via scroll spy
    const sections = $$("section[id], article[id]");
    const navLinks = {};
    $$(".sb-link").forEach(l => navLinks[l.dataset.nav] = l);
    const spy = new IntersectionObserver((entries) => {
      entries.forEach(en => {
        if (en.isIntersecting) {
          const id = en.target.id;
          // map quick-start cards & sub-ids to their nav entry
          const link = navLinks[id];
          if (link) {
            $$(".sb-link").forEach(l => l.classList.remove("active"));
            link.classList.add("active");
            link.scrollIntoView({ block: "nearest" });
          }
        }
      });
    }, { rootMargin: "-45% 0px -50% 0px" });
    sections.forEach(s => { if (navLinks[s.id]) spy.observe(s); });

    // scroll progress + back to top
    const prog = $("#scroll-progress");
    const top = $("#to-top");
    const onScroll = () => {
      const h = document.documentElement;
      const pct = h.scrollTop / (h.scrollHeight - h.clientHeight);
      prog.style.width = (pct * 100) + "%";
      top.classList.toggle("show", h.scrollTop > 600);
    };
    document.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }

  /* ============ COMMAND PALETTE ============ */
  let cmdItems = [], cmdSel = 0;
  function buildCmdk() {
    cmdItems = [];
    SPOUT.nav.forEach(g => g.items.forEach(it => cmdItems.push({ id: it.id, name: it.label, num: it.num || "·" })));
    SPOUT.api.forEach(n => cmdItems.push({ id: "api", name: "API · " + n.name, num: "fn" }));
    renderCmdk("");
  }
  function renderCmdk(q) {
    q = q.toLowerCase();
    const list = $("#cmdk-list");
    const filtered = cmdItems.filter(i => !q || i.name.toLowerCase().includes(q));
    cmdSel = 0;
    list.innerHTML = filtered.length ? filtered.map((i, idx) => `<div class="cmdk-item ${idx === 0 ? "sel" : ""}" data-go="${i.id}"><span class="ci-num mono">${i.num}</span><span class="ci-name">${i.name}</span></div>`).join("") : `<div class="cmdk-item"><span class="ci-name" style="color:var(--muted)">No matches</span></div>`;
    $$("#cmdk-list .cmdk-item").forEach(it => it.addEventListener("click", () => { goTo(it.dataset.go); closeCmdk(); }));
  }
  function openCmdk() { $("#cmdk-overlay").classList.add("open"); const inp = $("#cmdk-input-el"); inp.value = ""; renderCmdk(""); setTimeout(() => inp.focus(), 30); }
  function closeCmdk() { $("#cmdk-overlay").classList.remove("open"); }
  function goTo(id) {
    const el = document.getElementById(id);
    if (el) el.scrollIntoView({ behavior: reduced ? "auto" : "smooth", block: "start" });
    history.replaceState(null, "", "#" + id);
  }
  function initCmdkEvents() {
    const inp = $("#cmdk-input-el");
    inp.addEventListener("input", () => renderCmdk(inp.value));
    document.addEventListener("keydown", (e) => {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") { e.preventDefault(); $("#cmdk-overlay").classList.contains("open") ? closeCmdk() : openCmdk(); return; }
      if (!$("#cmdk-overlay").classList.contains("open")) return;
      const items = $$("#cmdk-list .cmdk-item[data-go]");
      if (e.key === "Escape") closeCmdk();
      else if (e.key === "ArrowDown") { e.preventDefault(); cmdSel = Math.min(cmdSel + 1, items.length - 1); updateSel(items); }
      else if (e.key === "ArrowUp") { e.preventDefault(); cmdSel = Math.max(cmdSel - 1, 0); updateSel(items); }
      else if (e.key === "Enter" && items[cmdSel]) { goTo(items[cmdSel].dataset.go); closeCmdk(); }
    });
    $("#cmdk-overlay").addEventListener("click", (e) => { if (e.target.id === "cmdk-overlay") closeCmdk(); });
  }
  function updateSel(items) { items.forEach((it, i) => it.classList.toggle("sel", i === cmdSel)); items[cmdSel]?.scrollIntoView({ block: "nearest" }); }

  /* ============ BACKGROUND: flowing pixels ============ */
  function initBackground() {
    if (reduced) { document.body.dataset.bg = "off"; return; }
    const c = $("#bg-canvas"); const ctx = c.getContext("2d");
    let w, h, dpr, particles = [];
    function resize() {
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      w = c.width = innerWidth * dpr; h = c.height = innerHeight * dpr;
      c.style.width = innerWidth + "px"; c.style.height = innerHeight + "px";
    }
    resize(); addEventListener("resize", resize);
    const N = Math.min(46, Math.floor(innerWidth / 26));
    for (let i = 0; i < N; i++) particles.push({ x: Math.random() * w, y: Math.random() * h, s: (Math.random() * 1.4 + 0.4) * dpr, sp: (Math.random() * 0.3 + 0.12) * dpr, len: Math.random() * 40 + 14 });
    const accent = () => getComputedStyle(document.documentElement).getPropertyValue("--accent-rgb").trim() || "60 216 232";
    function tick() {
      ctx.clearRect(0, 0, w, h);
      const rgb = accent().split(" ").join(",");
      particles.forEach(p => {
        p.x += p.sp;
        if (p.x > w + p.len) { p.x = -p.len; p.y = Math.random() * h; }
        const g = ctx.createLinearGradient(p.x - p.len, p.y, p.x, p.y);
        g.addColorStop(0, `rgba(${rgb},0)`); g.addColorStop(1, `rgba(${rgb},0.5)`);
        ctx.strokeStyle = g; ctx.lineWidth = p.s;
        ctx.beginPath(); ctx.moveTo(p.x - p.len, p.y); ctx.lineTo(p.x, p.y); ctx.stroke();
      });
      raf = requestAnimationFrame(tick);
    }
    let raf = requestAnimationFrame(tick);
  }

  /* ============ HERO PACKETS ============ */
  function initHeroPackets() {
    if (reduced) return;
    const g = $(".packets"); if (!g) return;
    const NS = "http://www.w3.org/2000/svg";
    // packets travel along y=150 from x=60 to x=595
    const lanes = 3;
    for (let i = 0; i < lanes; i++) {
      const r = document.createElementNS(NS, "rect");
      r.setAttribute("width", "7"); r.setAttribute("height", "7"); r.setAttribute("rx", "1.5");
      r.setAttribute("y", "146.5"); r.setAttribute("class", "hero-packet");
      g.appendChild(r);
      animatePacket(r, i * 900);
    }
    function animatePacket(r, delay) {
      const dur = 2600;
      function run(start) {
        function frame(now) {
          const t = ((now - start) % dur) / dur;
          const x = 60 + t * 535;
          r.setAttribute("x", x - 3.5);
          r.setAttribute("opacity", t < 0.08 ? t / 0.08 : t > 0.92 ? (1 - t) / 0.08 : 1);
          requestAnimationFrame(frame);
        }
        requestAnimationFrame(frame);
      }
      setTimeout(() => run(performance.now()), delay);
    }
  }

  /* ============ TWEAKS ============ */
  function initTweaks() {
    const saved = JSON.parse(localStorage.getItem("spout-tweaks") || "{}");
    if (saved.accent) document.documentElement.dataset.accent = saved.accent;
    if (saved.bg === "off") document.body.dataset.bg = "off";
    if (saved.motion === "calm") document.body.classList.add("calm");

    $("#tweaks-toggle").addEventListener("click", () => $("#tweaks").classList.toggle("open"));
    $$("#tweaks [data-accent-opt]").forEach(b => {
      b.classList.toggle("active", (document.documentElement.dataset.accent || "cyan") === b.dataset.accentOpt);
      b.addEventListener("click", () => {
        document.documentElement.dataset.accent = b.dataset.accentOpt;
        $$("#tweaks [data-accent-opt]").forEach(x => x.classList.remove("active"));
        b.classList.add("active"); save();
      });
    });
    $$("#tweaks [data-bg-opt]").forEach(b => {
      b.classList.toggle("active", (document.body.dataset.bg === "off" ? "off" : "on") === b.dataset.bgOpt);
      b.addEventListener("click", () => {
        if (b.dataset.bgOpt === "off") document.body.dataset.bg = "off"; else { delete document.body.dataset.bg; if (!reduced) initBackgroundOnce(); }
        $$("#tweaks [data-bg-opt]").forEach(x => x.classList.remove("active")); b.classList.add("active"); save();
      });
    });
    function save() {
      localStorage.setItem("spout-tweaks", JSON.stringify({
        accent: document.documentElement.dataset.accent || "cyan",
        bg: document.body.dataset.bg === "off" ? "off" : "on"
      }));
    }
  }
  let bgStarted = false;
  function initBackgroundOnce() { /* canvas keeps running; just ensure visible */ }

  /* ============ GO ============ */
  document.addEventListener("DOMContentLoaded", () => {
    mount();
    initCmdkEvents();
    // open hash target after mount
    if (location.hash) { const el = document.getElementById(location.hash.slice(1)); if (el) setTimeout(() => el.scrollIntoView(), 100); }
  });
})();
