/* SpoutPlugin Docs — reusable component builders (return HTML strings) */
window.C = (function () {
  const esc = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

  /* lightweight C++ signature highlighter */
  function highlightSig(sig) {
    let h = esc(sig);
    // comments
    h = h.replace(/(\/\/[^\n]*)/g, '<span class="tok-com">$1</span>');
    // types
    const types = ["bool", "void", "int32", "FName", "UTextureRenderTarget2D", "UMaterialInterface", "UMaterialInstanceDynamic", "UTexture2D", "ESpoutSendTextureFrom", "ESpoutType", "EPixelFormat", "TArray", "FSenderStruct", "const"];
    types.forEach(t => {
      h = h.replace(new RegExp("\\b" + t + "\\b", "g"), `<span class="tok-type">${t}</span>`);
    });
    // function name (first word before "(")
    h = h.replace(/(>|\s)([A-Za-z_][A-Za-z0-9_]*)(\()/, (m, a, fn, b) => `${a}<span class="tok-fn">${fn}</span>${b}`);
    return h;
  }

  /* figure: screenshot placeholder. data = figures[id] */
  function figure(id) {
    const f = SPOUT.figures[id];
    if (!f) return "";
    const sp = (SPOUT.figureSpecs && SPOUT.figureSpecs[id]) || { ar: "16:9", res: "1920 × 1080" };
    const arCss = sp.ar.split(":").join(" / ");
    const opt = f.optional
      ? '<span class="fig-opt optional">Optional</span>'
      : '<span class="fig-opt">Required</span>';
    return `
    <figure class="figure" data-fig="${id}">
      <div class="figure-head">
        <span class="fig-id">${id}</span>
        <span class="fig-file">${SPOUT.imageBase}${f.file}</span>
        ${opt}
      </div>
      <div class="figure-body">
        <div class="fig-frame" style="aspect-ratio:${arCss}">${ICON.image(20)}<span class="fig-ar mono">${sp.ar}</span></div>
        <figcaption>
          <div class="fig-cap-title">${f.title}</div>
          <div class="fig-cap-desc">${f.desc}</div>
          <div class="fig-specs">
            <span class="fig-spec"><b>Aspect</b>${sp.ar}</span>
            <span class="fig-spec"><b>Advised</b>${sp.res}</span>
            ${sp.note ? `<span class="fig-spec ghost">${sp.note}</span>` : ""}
          </div>
          <span class="fig-hint">${ICON.download(13)} drop ${f.file} here later</span>
        </figcaption>
      </div>
    </figure>`;
  }

  function code(sig, opts) {
    opts = opts || {};
    const inner = opts.raw ? esc(sig) : highlightSig(sig);
    return `<div class="code"><button class="copy-btn" data-copy="${esc(sig).replace(/"/g, "&quot;")}">copy</button><code>${inner}</code></div>`;
  }

  function callout(kind, title, body, icon) {
    const ic = icon || (kind === "warn" ? ICON.warn(20) : kind === "error" ? ICON.warn(20) : kind === "tip" ? ICON.bolt(20) : ICON.info(20));
    return `<div class="callout ${kind}"><span class="ic">${ic}</span><div class="c-body">${title ? `<span class="c-title">${title}</span>` : ""}${body}</div></div>`;
  }

  function steps(items) {
    return `<ol class="steps">${items.map((it, i) => {
      const n = `<span class="st-num mono">${String(i + 1).padStart(2, "0")}</span>`;
      if (typeof it === "string") return `<li>${n}<span class="st-title">${it}</span></li>`;
      return `<li>${n}<span class="st-title">${it.t}</span>${it.note ? `<div class="st-note">${it.note}</div>` : ""}</li>`;
    }).join("")}</ol>`;
  }

  function secHead(num, eyebrow, title, intro) {
    return `
    <div class="sec-head reveal">
      <span class="sec-eyebrow"><span class="idx">${num}</span>${eyebrow}</span>
      <h2 class="sec-title">${title}<a class="anchor-link" data-anchor>#</a></h2>
      ${intro ? `<p class="sec-intro">${intro}</p>` : ""}
    </div>`;
  }

  function section(id, inner) {
    return `<section class="section" id="${id}" data-screen-label="${id}">${inner}</section>`;
  }

  function flow(nodes, reverse) {
    const parts = [];
    nodes.forEach((n, i) => {
      parts.push(`<div class="flow-node"><div class="fn-label">${n.label}</div><div class="fn-name">${n.name}</div>${n.sub ? `<div class="fn-sub">${n.sub}</div>` : ""}</div>`);
      if (i < nodes.length - 1) parts.push(`<div class="flow-arrow"><span class="pkt"></span></div>`);
    });
    return `<div class="flow ${reverse ? "reverse" : ""}">${parts.join("")}</div>`;
  }

  function leadList(items) {
    return `<ul class="lead-list">${items.map(i => `<li>${i}</li>`).join("")}</ul>`;
  }

  return { esc, figure, code, callout, steps, secHead, section, flow, leadList, highlightSig };
})();
