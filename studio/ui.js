"use strict";

const UI = (() => {
  const icons = {
    monitor: '<rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8m-4-4v4"/>',
    window: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9h18"/>',
    terminal: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="m7 9 3 3-3 3m6 0h4"/>',
    video: '<rect x="3" y="5" width="13" height="14" rx="2"/><path d="m16 10 5-3v10l-5-3"/>',
    camera: '<path d="M3 7h4l2-3h6l2 3h4v13H3z"/><circle cx="12" cy="13" r="3"/>',
    film: '<rect x="3" y="3" width="18" height="18" rx="2"/><path d="M7 3v18M17 3v18M3 9h4m10 0h4M3 15h4m10 0h4"/>',
    layers: '<path d="m12 3 9 5-9 5-9-5 9-5Zm-9 9 9 5 9-5M3 16l9 5 9-5"/>',
    image: '<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 16-5-5-9 9"/>',
    calendar: '<rect x="3" y="5" width="18" height="16" rx="2"/><path d="M3 10h18m-6-7v4m-6-4v4"/><path d="M12 13v3l2 1"/>',
    float: '<rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 10h18m-11 4h7v2h-7z"/>',
    cog: '<path d="M10 3h4l.7 3 2 .9L19.5 6l2 3.5-2.1 2 .1 2.2 2 2-2 3.5-2.8-.9-2 .9-.7 2.8h-4l-.7-2.8-2-.9-2.8.9-2-3.5 2-2 .1-2.2-2.1-2 2-3.5 2.8.9 2-.9z"/><circle cx="12" cy="12" r="3"/>',
    download: '<path d="M12 3v12m-4-4 4 4 4-4M4 15v5h16v-5"/>',
    bot: '<rect x="5" y="8" width="14" height="12" rx="2"/><path d="M12 4v4M2 12v4m20-4v4m-13-3v1m6-1v1m-5 3h4"/>',
    spark: '<path d="M10 3c1 6 3 8 9 9-6 1-8 3-9 9-1-6-3-8-9-9 6-1 8-3 9-9Zm9 0v6m-3-3h6m-2 10v5m-2.5-2.5h5"/>',
    minus: '<path d="M5 12h14"/>', plus: '<path d="M5 12h14m-7-7v14"/>',
    square: '<rect x="6" y="6" width="12" height="12" rx="1"/>',
    restore: '<path d="M8 6V4h12v12h-2"/><rect x="4" y="8" width="12" height="12" rx="1"/>',
    close: '<path d="m6 6 12 12M18 6 6 18"/>',
    full: '<path d="M9 4H4v5m11-5h5v5m0 6v5h-5m-11-5v5h5"/>',
    chevron: '<path d="m6 9 6 6 6-6"/>', right: '<path d="m9 6 6 6-6 6"/>',
    flag: '<path d="M5 21V4m0 1c4-4 8 4 14 0v10c-6 4-10-4-14 0"/>',
    search: '<circle cx="10.5" cy="10.5" r="5.5"/><path d="m15 15 5 5"/>',
    eye: '<path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6S2 12 2 12Z"/><circle cx="12" cy="12" r="2.5"/>',
    hidden: '<path d="m3 3 18 18M12 6c6.5 0 10 6 10 6a16 16 0 0 1-3.7 3.8M6 7.7A17 17 0 0 0 2 12s3.5 6 10 6c1.4 0 2.7-.3 3.8-.7"/>',
    mic: '<rect x="9" y="3" width="6" height="11" rx="3"/><path d="M6 10v2a6 6 0 0 0 12 0v-2m-6 8v3m-4 0h8"/>',
    "mic-off": '<path d="M9 9v2a3 3 0 0 0 5.1 2.1M9.3 4.7A3 3 0 0 1 15 6v4M6 10v2a6 6 0 0 0 9 5.2M18 10v2a6 6 0 0 1-.4 2.2M12 18v3m-4 0h8M3 3l18 18"/>',
    volume: '<path d="M3 9h4l5-4v14l-5-4H3zM16 8a5 5 0 0 1 0 8m3-11a9 9 0 0 1 0 14"/>',
    mute: '<path d="M3 9h4l5-4v14l-5-4H3zm13 0 6 6m0-6-6 6"/>',
    wave: '<path d="M2 12h3l2-5 3 10 4-12 3 11 2-4h3"/>',
    record: '<circle cx="12" cy="12" r="6" fill="currentColor" stroke="none"/>',
    stop: '<rect x="6" y="6" width="12" height="12" rx="1.5" fill="currentColor" stroke="none"/>',
    pause: '<path d="M9 6v12m6-12v12" stroke-width="3"/>',
    play: '<path d="m8 4 12 8-12 8z" fill="currentColor" stroke="none"/>',
    previous: '<path d="M5 5v14m13-14L8 12l10 7z" fill="currentColor"/>',
    next: '<path d="M19 5v14M6 5l10 7-10 7z" fill="currentColor"/>',
    folder: '<path d="M3 7V5h6l3 3h9v12H3V7z"/><path d="M3 10h18"/>',
    trash: '<path d="M4 6h16M9 6V3h6v3m3 0-1 15H7L6 6m4 4v7m4-7v7"/>',
    save: '<path d="M4 3h13l3 3v15H4zM8 3v6h8V3M8 21v-8h8v8"/>',
    book: '<path d="M12 5c-3-2-6-2-9-1v15c3-1 6-1 9 1 3-2 6-2 9-1V4c-3-1-6-1-9 1Zm0 0v15"/>',
    link: '<path d="M9 15l6-6m-6 9-2 2a4 4 0 0 1-5-6l4-4a4 4 0 0 1 6 0m3-4 2-2a4 4 0 0 1 5 6l-4 4a4 4 0 0 1-6 0"/>',
    lock: '<rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3m-4 4v3"/>',
    unlock: '<rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 7-2.5m-3 9.5v3"/>',
    scissors: '<circle cx="5.5" cy="6" r="3"/><circle cx="5.5" cy="18" r="3"/><path d="M8 8l13 12M8 16 21 4"/>',
    undo: '<path d="M3 9h11a5 5 0 0 1 0 10h-3M8 4 3 9l5 5"/>',
    redo: '<path d="M21 9H10a5 5 0 0 0 0 10h3m3-15 5 5-5 5"/>',
    magnet: '<path d="M5 4h4v9a3 3 0 0 0 6 0V4h4v9a7 7 0 1 1-14 0V4zm0 4h4m6 0h4"/>',
    text: '<path d="M5 6V4h14v2M12 4v16m-4 0h8"/>',
    crop: '<path d="M6 2v16h16M2 6h16v16"/>',
    collapse: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M9 4v16m7-11-3 3 3 3"/>',
    refresh: '<path d="M20 9a8 8 0 0 0-14-4L3 8m0-5v5h5m-4 7a8 8 0 0 0 14 4l3-3m0 5v-5h-5"/>',
    chip: '<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 9h6v6H9zm0-7v3m6-3v3m-6 14v3m6-3v3M2 9h3m-3 6h3m14-6h3m-3 6h3"/>',
    disk: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 14h18m-4 3h1m-5 0h1"/>',
    check: '<path d="m5 12 4 4L19 6"/>',
    pen: '<path d="M4 20l4-1 12-12-3-3L5 16l-1 4zM14 7l3 3"/>',
    pointer: '<path d="M4 3l4.3 17 4.1-6.6 7.6-2.9L4 3z"/>',
    nib: '<path d="m9 3 8 2.4 3.6 8.8-6.4 6.4-8.8-3.6L3 9Zm-6 6 8 5m3-3L9 3"/><circle cx="13" cy="13" r="2"/>',
    highlighter: '<path d="m5 17 1-5 9-9 6 6-9 9-5 1-3 2-1-1Zm1-5 6 6M3 21h11"/>',
    eraser: '<path d="m5.8 18.2-2.4-2.4a2 2 0 0 1 0-2.8l8.7-8.7a2 2 0 0 1 2.8 0l5.7 5.7a2 2 0 0 1 0 2.8l-5.4 5.4H5.8Zm1-8.6 8.6 8.6M11 18.2h10"/>',
    board: '<path d="M3 4h18M5 4v11h14V4m-7 11v6m-4 0h8"/>',
    grip: '<path d="M9 5h.01m6 0h.01M9 12h.01m6 0h.01M9 19h.01m6 0h.01" stroke-width="3"/>',
    "stroke-width": '<path d="M4 6h16M4 12h16" /><path d="M4 18h16" stroke-width="3"/>',
    "blur-strength": '<path d="M4 7h5m4 0h7M4 17h9m4 0h3M9 4v6m8 4v6"/>',
    "click-effect": '<path d="M12 3a9 9 0 1 1-9 9" stroke-dasharray="2 3"/><circle cx="12" cy="12" r="3.5"/>',
    up: '<path d="m6 15 6-6 6 6"/>',
    arrow: '<path d="M5 19 19 5m-9 0h9v9"/>',
    blur: '<path d="M4 4h16v16H4zM4 9h16M4 15h16M9 4v16m6-16v16"/>',
    mask: '<path d="M12 2 4 5v6c0 5 8 11 8 11s8-6 8-11V5z" fill="currentColor"/>',
    send: '<path d="m3 11 18-8-5 18-4-7-9-3Zm9 3 9-11"/>',
    palette: '<path d="M12 3a9 9 0 0 0 0 18h1.4a2.3 2.3 0 0 0 1.6-3.9 2.3 2.3 0 0 1 1.6-3.9H18a3 3 0 0 0 3-3A7.2 7.2 0 0 0 12 3Z"/><circle cx="7.8" cy="11.5" r="1" fill="currentColor"/><circle cx="10.5" cy="7.8" r="1" fill="currentColor"/><circle cx="15.2" cy="8.2" r="1" fill="currentColor"/>',
  };

  const glyph = name => `<svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${icons[name] || ""}</svg>`;
  const escape = value => String(value ?? "").replace(/[&<>"']/g, char =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[char]);
  const clamp = (number, min, max) => Math.min(max, Math.max(min, number));
  let catalog = {};
  const text = label => Object.hasOwn(catalog, label) ? catalog[label] : label;
  function setLanguage(locale, messages) {
    document.documentElement.lang = locale;
    catalog = locale === "en" ? { ...messages, ...studioEnglish } : messages || {};
  }
  const time = (seconds, hours = false) => {
    const elapsed = Math.max(0, Math.floor(seconds));
    const part = number => String(number).padStart(2, "0");
    const hh = Math.floor(elapsed / 3600);
    const mm = Math.floor(elapsed / 60) % 60;
    return `${hours || hh ? part(hh) + ":" : ""}${part(mm)}:${part(elapsed % 60)}`;
  };

  function button(action, label, icon, opts = {}) {
    const { variant = "", value, disabled, iconOnly = false, pressed, optional = false, shortcut } = opts;
    const caption = opts.translate === false ? label : text(label);
    return `<button type="button" class="btn ${variant}${iconOnly ? " icon-btn" : ""}${optional ? " optional" : ""}"
      data-action="${action}"${value == null ? "" : ` data-value="${escape(value)}"`}
      ${disabled ? "disabled" : ""}${pressed == null ? "" : ` aria-pressed="${pressed}"`}
      ${iconOnly || optional ? ` aria-label="${escape(caption)}" title="${escape(caption)}"` : ""}>
      ${icon ? glyph(icon) : ""}${iconOnly ? "" : `<span class="${optional ? "optional-label" : ""}">${escape(caption)}</span>`}
      ${shortcut ? `<kbd>${escape(shortcut)}</kbd>` : ""}</button>`;
  }

  const iconButton = (action, label, icon, opts = {}) =>
    button(action, label, icon, { variant: "flat mini", ...opts, iconOnly: true });

  function head(label, icon, right = "", extra = "", attributes = "") {
    return `<div class="panel-head"${attributes}><h2 class="section-title">${glyph(icon)}${escape(text(label))}${extra}</h2>${right}</div>`;
  }

  function tabs(action, items, current, label) {
    return `<div class="segmented" aria-label="${escape(label)}">${items.map(([id, name, icon]) =>
      button(action, name, icon, { value: id, variant: "flat", pressed: id === current })).join("")}</div>`;
  }

  function select(key, label, values, value, extra = "") {
    const options = values.map(item => {
      const [id, name] = Array.isArray(item) ? item : [item, item];
      return `<option value="${escape(id)}"${String(id) === String(value) ? " selected" : ""}>${escape(text(name))}</option>`;
    }).join("");
    return `<select data-field="${key}" aria-label="${escape(text(label))}" ${extra}>${options}</select>`;
  }

  const jpg = name => `assets/${name}.jpg`;
  const frame = sec => jpg(`shot-${[0, 18, 40, 64, 94, 118].findLastIndex(start => sec >= start)}`);
  const pct = (value, total) => `${clamp(100 * value / (total || 1), 0, 100)}%`;

  return { glyph, escape, clamp, time, button, iconButton, head, tabs, select, jpg, frame, pct, text, setLanguage };
})();
