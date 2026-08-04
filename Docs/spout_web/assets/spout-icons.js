// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/* SpoutPlugin Docs — inline SVG icons (stroke=currentColor) */
window.ICON = (function () {
  const s = (p, w) => `<svg width="${w||18}" height="${w||18}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${p}</svg>`;
  return {
    search: (w) => s('<circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/>', w),
    chevron: (w) => s('<path d="M6 9l6 6 6-6"/>', w),
    chevronR: (w) => s('<path d="M9 6l6 6-6 6"/>', w),
    arrowUp: (w) => s('<path d="M12 19V5M5 12l7-7 7 7"/>', w),
    copy: (w) => s('<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/>', w),
    check: (w) => s('<path d="M20 6L9 17l-5-5"/>', w),
    bolt: (w) => s('<path d="M13 2L4.5 13H11l-1 9 8.5-11H12l1-9z"/>', w),
    warn: (w) => s('<path d="M12 9v4M12 17h.01M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/>', w),
    info: (w) => s('<circle cx="12" cy="12" r="9"/><path d="M12 16v-4M12 8h.01"/>', w),
    image: (w) => s('<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8.5" cy="8.5" r="1.5"/><path d="M21 15l-5-5L5 21"/>', w),
    menu: (w) => s('<path d="M3 6h18M3 12h18M3 18h18"/>', w),
    x: (w) => s('<path d="M18 6L6 18M6 6l12 12"/>', w),
    send: (w) => s('<path d="M22 2 11 13M22 2l-7 20-4-9-9-4 20-7z"/>', w),
    download: (w) => s('<path d="M12 3v12M7 10l5 5 5-5M5 21h14"/>', w),
    receive: (w) => s('<path d="M2 22 13 11M2 22l7-20 4 9 9 4-20 7z"/>', w),
    cpu: (w) => s('<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v2M15 2v2M9 20v2M15 20v2M2 9h2M2 15h2M20 9h2M20 15h2"/>', w),
    layers: (w) => s('<path d="M12 2 2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/>', w),
    link: (w) => s('<path d="M10 13a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1M14 11a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"/>', w),
    monitor: (w) => s('<rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8M12 17v4"/>', w),
    target: (w) => s('<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1"/>', w),
    terminal: (w) => s('<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/>', w),
    gauge: (w) => s('<path d="M12 13l4-4M3.5 19a9 9 0 1 1 17 0"/>', w),
    box: (w) => s('<path d="M21 8l-9-5-9 5v8l9 5 9-5V8zM3 8l9 5 9-5M12 13v8"/>', w),
    book: (w) => s('<path d="M4 4v15a1 1 0 0 0 1 1h14M6 4h13v13H6a2 2 0 0 0-2 2V6a2 2 0 0 1 2-2z"/>', w),
    flask: (w) => s('<path d="M9 3h6M10 3v6L5 19a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1l-5-10V3"/>', w),
    folder: (w) => s('<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7z"/>', w),
    wrench: (w) => s('<path d="M14.7 6.3a4 4 0 0 0-5.4 5.2L3 18l3 3 6.5-6.3a4 4 0 0 0 5.2-5.4l-2.6 2.6-2.7-.4-.4-2.7 2.6-2.5z"/>', w),
    shield: (w) => s('<path d="M12 2 4 5v6c0 5 3.4 8.5 8 11 4.6-2.5 8-6 8-11V5l-8-3z"/>', w),
    list: (w) => s('<path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/>', w),
    sliders: (w) => s('<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>', w),
    swatch: (w) => s('<path d="M4 4h7v16a3 3 0 0 1-6 0V4z"/><path d="M11 11l4-4 5 5-9 9"/>', w),
    grid: (w) => s('<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>', w),
    pulse: (w) => s('<path d="M2 12h4l3 8 4-16 3 8h6"/>', w)
  };
})();
