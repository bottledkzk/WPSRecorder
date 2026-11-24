"use strict";

const { button: b, iconButton: ib, glyph: g, time, escape: esc, select, tabs, clamp, pct, jpg } = UI;
const tr = label => esc(UI.text(label));
const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

const sourceCatalog = {
  screen: { label: "屏幕", icon: "monitor", targets: [] },
  region: { label: "区域", icon: "crop", targets: [] },
  window: { label: "窗口", icon: "window", targets: [] },
  camera: { label: "摄像头", icon: "video", targets: [] },
  image: { label: "图片", icon: "image", targets: [] },
};

function makeSource(type, target = 0, visible = true) {
  return { id: crypto.randomUUID(), type, target, visible };
}

function makeScene(name, sources = []) {
  return { name, sources, selected: sources[0]?.id ?? "" };
}

const dockLabels = { scene: "场景", source: "源", mixer: "混音器", output: "录制设置", control: "录制控制" };
const defaultDockOrder = Object.keys(dockLabels);
const defaultDockWidths = [340, 385, 267, 509, 314];
const dockKey = "wps-recorder-alignment-docks-v2";
function readDockPreferences() {
  try {
    return JSON.parse(localStorage.getItem(dockKey)) ?? {};
  } catch {
    return {};
  }
}

const dockPreferences = readDockPreferences();
function readDockLayout({ order, widths }) {
  const usable = Array.isArray(widths) && widths.every(value => Number.isFinite(value) && value > 0);
  if (usable && widths.length === defaultDockOrder.length && Array.isArray(order)
    && order.length === defaultDockOrder.length && defaultDockOrder.every(id => order.includes(id))) {
    return { order, widths };
  }
  const previous = ["scene", "source", "mixer", "control", "output"];
  if (usable && widths.length === previous.length && widths.join() !== "13,17,23,23,24") {
    return { order: defaultDockOrder.slice(), widths: defaultDockOrder.map(id => widths[previous.indexOf(id)]) };
  }
  return { order: defaultDockOrder.slice(), widths: defaultDockWidths.slice() };
}
const dockLayout = readDockLayout(dockPreferences);
const mixerSources = {
  mic: { name: "麦克风", icon: "mic", peak: 0, db: "−∞ dB" },
  system: { name: "系统播放声", icon: "volume", peak: 0, db: "−∞ dB" },
};
const countdownOptions = [[0, "立即开始"], [3, "3 秒"], [5, "5 秒"], [10, "10 秒"]];
const defaultAssistantWidth = 316;

const emptyAsset = { id: "", title: "未选择视频", name: "", duration: 0, frameRate: 30, marks: [], thumbnails: [] };
const assets = [];

const state = {
  page: "record", minimized: false, closed: false,
  scene: 0,
  scenes: [makeScene("场景 1")],
  sceneEdit: "", dockOrder: dockLayout.order,
  zoom: 0, captureAt: 0, width: { ai: 34,
    docks: dockLayout.widths,
  }, height: { docks: 221 },
  settings: { resolution: 1080, fps: 60, bitrate: 6000, customBitrate: false, encoder: "自动",
    color: "SDR", format: "MP4", outputPath: "", outputPathIsFolderName: false,
    countdown: 3, audioMode: "双独立音轨", denoise: true,
    language: "简体中文" },
  audio: { mic: { volume: 100, muted: false }, system: { volume: 72, muted: false } },
  record: { phase: "idle", seconds: 0, countdown: 0, marks: [] },
  asset: "", review: { query: "", filter: "all", collapsed: false, at: 0, playing: false, volume: 85, speed: 1 },
  ttsServices: [], editorAsset: "", editorLayout: { left: 209, right: 315, timeline: 34 }, editor: {
    playing: false, volume: 85, speed: 1, bin: "media", tab: "subtitle", crop: "", cropRatio: { width: 4, height: 3 },
    stills: [], previewStill: "",
    subtitleTrack: "T1", subtitleQuery: "", subtitleSize: 20, subtitleColor: "#ffffff",
    subtitleFont: "Microsoft YaHei", subtitleOpacity: 100,
    asrTrack: "A1", asrLanguage: "简体中文", direction: "中 → 英", glossary: "",
    dub: { mode: "sentences", service: "local", engine: "", voice: "普通话女声", speed: 1, volume: 95,
      textType: {}, scripts: {}, rules: {}, narration: "", narrationRule: "", chapterScripts: {}, chapterRules: {},
      duck: true, generated: false },
  },
  ai: { collapsed: false, hide: "" },
  assistant: { record: { open: false, width: defaultAssistantWidth }, edit: { open: false, width: defaultAssistantWidth } },
  floating: false, floatSmall: false, floatPosition: null,
  annotation: {
    open: false, collapsed: false, tool: "pen", color: "#fa6262", size: 4, blur: 12,
    effect: "", whiteboard: false, clickStyle: "ripple", point: null, flash: null,
    toolbarPosition: null,
    strokes: [], draft: null, undo: [], redo: [],
  },
  schedule: [],
  telemetry: {},
  liveFrame: null,
};

const annotationOptions = {
  size: { label: "画笔粗细", icon: "stroke-width", items: [[2, "2 px"], [4, "4 px"], [7, "7 px"]] },
  blur: { label: "模糊程度", icon: "blur-strength", items: [[6, "轻度"], [12, "中度"], [22, "重度"]] },
  clickStyle: { label: "点击效果", icon: "click-effect",
    items: [["none", "无"], ["ripple", "涟漪"], ["ring", "光圈"], ["highlight", "高光"]] },
};

const timeline = new Timeline(timelineChange, at => seekPlayer("editor", at), toast);
timeline.resolveStill = editorStillItem;
timeline.reset(0, "", [], 30);
timeline.tracks[0].clips = [];
const dialog = $("#dialog");
let dialogName = "";
let exportDraft;
let toastTimer;
let frameHandle;
let editorTick = 0;
let activeChoice;
const previewResize = new ResizeObserver(() => updatePreviewRatio());

function activeScene() {
  return state.scenes[state.scene];
}

function activeSource() {
  const scene = activeScene();
  return scene.sources.find(item => item.id === scene.selected) ?? scene.sources.at(-1);
}

function targetFor(item) {
  const targets = sourceCatalog[item.type].targets;
  const label = item.type === "image" ? item.label : targets[item.target]?.[0] || item.label;
  return [label || sourceCatalog[item.type].label, item.device];
}

function dockColumns() {
  return state.width.docks.map(width => `minmax(0, ${width}fr)`).join(" 3px ");
}

function dockDividerLabel(index) {
  return "调整" + dockLabels[state.dockOrder[index]] + "与" + dockLabels[state.dockOrder[index + 1]] + "宽度";
}

function saveDockPreferences() {
  try {
    localStorage.setItem(dockKey, JSON.stringify({
      order: state.dockOrder, widths: state.width.docks,
    }));
  } catch {
    // The current workspace still works when browser storage is unavailable.
  }
}

function toast(message) {
  $("#toast").textContent = message;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { $("#toast").textContent = ""; }, 2900);
}

function resolutionLabel() {
  const { resolution, width, height } = state.settings;
  if (resolution === -1 && width > 0 && height > 0) return width + "×" + height;
  return ({ 0: state.telemetry.size || "自动", 720: "1280×720", 1080: "1920×1080", 1440: "2560×1440", 2160: "3840×2160" })[resolution] || "自动";
}

function recommendedBitrate() {
  const factors = { 720: 0.65, 1080: 1, 1440: 1.6, 2160: 2.7 };
  return Math.round(6000 * factors[state.settings.resolution] * (state.settings.fps / 60) ** 0.69 / 100) * 100;
}

function chrome() {
  $("#titlebar").innerHTML = Views.titlebar();
  $("#statusbar").innerHTML = Views.status();
  renderFloating();
}

function placeFloatingBar(bar = $("#floating .floating-bar")) {
  if (!bar || !state.floatPosition) return;
  const { x, y } = state.floatPosition;
  const left = clamp(x * innerWidth - bar.offsetWidth / 2, 6, Math.max(6, innerWidth - bar.offsetWidth - 6));
  const top = clamp(y * innerHeight - bar.offsetHeight / 2, 6, Math.max(6, innerHeight - bar.offsetHeight - 6));
  bar.classList.add("placed");
  bar.style.left = Math.round(left) + "px";
  bar.style.top = Math.round(top) + "px";
}

function renderFloating() {
  const host = $("#floating");
  host.innerHTML = Views.floating();
  const bar = $(".floating-bar", host);
  if (!bar) return;
  placeFloatingBar(bar);

  const moveTo = (x, y) => {
    const marginX = Math.min(.5, (6 + bar.offsetWidth / 2) / innerWidth);
    const marginY = Math.min(.5, (6 + bar.offsetHeight / 2) / innerHeight);
    state.floatPosition = { x: clamp(x / innerWidth, marginX, 1 - marginX), y: clamp(y / innerHeight, marginY, 1 - marginY) };
    placeFloatingBar(bar);
  };

  let drag;
  let suppressClick = false;
  bar.addEventListener("pointerdown", event => {
    if (event.button !== 0 || !event.isPrimary) return;
    suppressClick = false;
    const button = event.target.closest("button");
    if (button && !button.hasAttribute("data-floating-grip")) return;
    const rect = bar.getBoundingClientRect();
    const capture = button || bar;
    drag = { id: event.pointerId, pointerX: event.clientX, pointerY: event.clientY,
      centerX: rect.x + rect.width / 2, centerY: rect.y + rect.height / 2 };
    capture.setPointerCapture(event.pointerId);
  });
  bar.addEventListener("pointermove", event => {
    if (drag?.id !== event.pointerId) return;
    const dx = event.clientX - drag.pointerX;
    const dy = event.clientY - drag.pointerY;
    if (!suppressClick && Math.hypot(dx, dy) < 4) return;
    suppressClick = true;
    bar.classList.add("dragging");
    moveTo(drag.centerX + dx, drag.centerY + dy);
  });
  bar.addEventListener("lostpointercapture", () => {
    drag = null;
    bar.classList.remove("dragging");
  });
  bar.addEventListener("click", event => {
    if (!suppressClick || !event.detail) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    suppressClick = false;
  }, true);

  bar.addEventListener("keydown", event => {
    if (!event.target.hasAttribute("data-floating-grip")) return;
    const offset = { ArrowLeft: [-16, 0], ArrowRight: [16, 0], ArrowUp: [0, -16], ArrowDown: [0, 16] }[event.key];
    if (!offset) return;
    event.preventDefault();
    event.stopPropagation();
    const rect = bar.getBoundingClientRect();
    moveTo(rect.x + rect.width / 2 + offset[0], rect.y + rect.height / 2 + offset[1]);
  });
}

function assistantPane() {
  if (state.page === "ai") return null;
  return state.assistant[state.page === "editor" ? "edit" : "record"];
}

function resetAssistantPanes() {
  for (const pane of Object.values(state.assistant)) {
    pane.open = false;
    pane.width = defaultAssistantWidth;
  }
}

function drawAssistant(refresh = false) {
  const aside = $("#assistant");
  const pane = assistantPane();
  aside.hidden = !pane?.open || state.closed || state.minimized;
  const layout = $("#app-main");
  layout.classList.toggle("has-assistant", !aside.hidden);
  layout.style.setProperty("--assistant-width", (pane?.width ?? defaultAssistantWidth) + "px");
  $("#assistant-divider").hidden = aside.hidden;
  $$("#workspace [data-action='sidebar-ai']").forEach(button => {
    button.setAttribute("aria-pressed", String(!aside.hidden));
  });
  if (aside.hidden) {
    aside.replaceChildren();
    delete aside.dataset.scope;
    return;
  }
  const scope = state.page === "editor" ? "edit:" + (hasExplicitEditorProject() ? AgentWorkspace.currentProject() : "") : "record";
  if (aside.dataset.scope === scope && !refresh) return;
  aside.dataset.scope = scope;
  aside.innerHTML = Views.assistant();
  const log = $(".chat-log", aside);
  if (log) log.scrollTop = log.scrollHeight;
}

function render() {
  actions["leave-full"]();
  closeChoiceMenu();
  previewResize.disconnect();
  const capture = $("#capture-video");
  if (capture?.readyState > 0) state.captureAt = capture.currentTime;
  cancelAnimationFrame(frameHandle);
  annotationToolbarObserver.disconnect();
  $(".annotation-toolbar")?.remove();
  const main = $("#workspace");
  const mountedCapture = $("#capture-image", main);
  const mountedPlayer = main.querySelector("video[data-asset-id]");
  const mountedTimeline = state.page === "editor" && timeline.root?.dataset.project === activeEditorProjectId() ? timeline.root : null;
  const scrollX = $(".tracks-viewport", mountedTimeline ?? main)?.scrollLeft ?? 0;
  const scrollY = $(".timeline-body", mountedTimeline ?? main)?.scrollTop ?? 0;
  const offlineShell = state.minimized || state.closed;
  main.innerHTML = offlineShell
    ? `<div class="mock-desktop">${b("restore-app", "打开 WPS Recorder", "video", { variant: "primary" })}</div>`
    : ({ record: Views.recordPage, review: Views.reviewPage, editor: Views.editorPage, ai: Views.aiPage })[state.page]();
  const nextCapture = $("#capture-image", main);
  if (mountedCapture?.naturalWidth > 0 && !mountedCapture.hidden && nextCapture?.src === mountedCapture.src)
    nextCapture.replaceWith(mountedCapture);
  const nextPlayer = main.querySelector("video[data-asset-id]");
  if (nextPlayer && mountedPlayer?.id === nextPlayer.id && mountedPlayer.src === nextPlayer.src
      && mountedPlayer.dataset.assetId === nextPlayer.dataset.assetId) nextPlayer.replaceWith(mountedPlayer);
  else if (mountedPlayer) {
    mountedPlayer.pause();
    mountedPlayer.removeAttribute("src");
    mountedPlayer.load();
  }
  chrome();
  drawAssistant();
  if (offlineShell) return;
  if (state.page === "record") {
    applyZoom();
    previewResize.observe($("#capture-frame"));
    const video = $("#capture-video");
    if (video) video.addEventListener("loadedmetadata", () => { video.currentTime = state.captureAt; }, { once: true });
    setupAnnotations();
  }
  if (state.page === "editor") {
    const root = $("#timeline");
    root.dataset.project = activeEditorProjectId();
    timeline.mount(root);
    if (mountedTimeline) {
      $(".tracks-viewport", root).scrollLeft = scrollX;
      $(".timeline-body", root).scrollTop = scrollY;
    }
  }
  if (["review", "editor"].includes(state.page)) wirePlayer(state.page);
  if (state.page === "ai") {
    const log = $(".chat-log", main);
    if (log) log.scrollTop = log.scrollHeight;
  }
}

let workspaceNavigationEpoch = 0;

function navigate(page) {
  workspaceNavigationEpoch++;
  Native.tell("navigate", { page });
  if (page !== state.page) {
    if (state.page === "editor") { persistEditorDraft(); stopEditorMonitor(true); }
    state.editor.playing = false;
    state.review.playing = false;
    state.page = page;
  }
  state.minimized = false;
  state.closed = false;
  render();
  if (page === "review") refreshReviewSelection();
}

let previewZoomFrame = 0;

function previewSourceSize() {
  const { width, height } = state.liveFrame || {};
  if (width > 0 && height > 0) return { width, height };
  const image = $("#capture-image");
  return { width: image?.naturalWidth || 1280, height: image?.naturalHeight || 720 };
}

function applyZoom(animate = false) {
  cancelAnimationFrame(previewZoomFrame);
  const fit = $(".record-monitor .media-fit");
  const frame = $("#capture-frame");
  const canvas = $("#record-canvas");
  if (!fit || !frame || !canvas) return;
  const { width, height } = previewSourceSize();
  const startWidth = frame.getBoundingClientRect().width;
  const centerX = canvas.scrollWidth > canvas.clientWidth
    ? (canvas.scrollLeft + canvas.clientWidth / 2) / canvas.scrollWidth : .5;
  const centerY = canvas.scrollHeight > canvas.clientHeight
    ? (canvas.scrollTop + canvas.clientHeight / 2) / canvas.scrollHeight : .5;
  fit.style.setProperty("--capture-aspect", width / height);
  fit.classList.toggle("zoomed", !!state.zoom);
  const target = state.zoom ? width * state.zoom / 100 : 0;
  const resize = pixels => {
    frame.style.width = pixels ? Math.round(pixels) + "px" : "";
    canvas.scrollLeft = pixels ? centerX * canvas.scrollWidth - canvas.clientWidth / 2 : 0;
    canvas.scrollTop = pixels ? centerY * canvas.scrollHeight - canvas.clientHeight / 2 : 0;
    updatePreviewRatio();
    drawAnnotations();
  };
  if (animate && target && startWidth > 0 && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
    const started = performance.now();
    resize(startWidth);
    const tick = now => {
      if (!frame.isConnected) return;
      const progress = clamp((now - started) / 140, 0, 1);
      resize(startWidth + (target - startWidth) * (1 - (1 - progress) ** 3));
      if (progress < 1) previewZoomFrame = requestAnimationFrame(tick);
    };
    previewZoomFrame = requestAnimationFrame(tick);
  } else resize(target);
  const options = $("[data-field='preview-fit']");
  if (!options) return;
  const zoom = String(state.zoom);
  if (![...options.options].some(option => option.value === zoom)) {
    if (options.options.length > 4) options.options[4].remove();
    options.add(new Option(UI.text("自定义缩放"), zoom));
  }
  options.value = zoom;
}

function updatePreviewRatio() {
  const size = $("#capture-frame")?.clientWidth;
  const output = $("#preview-ratio");
  if (size && output) output.textContent = Math.round(size / previewSourceSize().width * 100) + "%";
}

function closeChoiceMenu(restoreFocus = false) {
  if (!activeChoice) return;
  const { button, panel } = activeChoice;
  activeChoice = null;
  panel.remove();
  button.setAttribute("aria-expanded", "false");
  button.removeAttribute("aria-controls");
  if (restoreFocus && button.isConnected) button.focus({ preventScroll: true });
  if (previewFullscreen?.panel.contains(button)) previewFullscreen.revealControls?.();
}

function toggleChoiceMenu(button, items, current, pick) {
  const wasOpen = activeChoice?.button === button;
  closeChoiceMenu();
  if (wasOpen || !button || !items.length) return;

  const panel = document.createElement("div");
  panel.id = "recorder-choice-menu";
  panel.className = "choice-menu";
  panel.setAttribute("role", "listbox");
  panel.setAttribute("aria-label", button.getAttribute("aria-label") || button.textContent.trim());
  if (panel.showPopover) panel.setAttribute("popover", "manual");
  const options = items.map(([value, label]) => {
    const option = document.createElement("button");
    option.type = "button";
    option.setAttribute("role", "option");
    option.setAttribute("aria-selected", String(value) === String(current) ? "true" : "false");
    option.tabIndex = -1;
    option.textContent = UI.text(label);
    option.addEventListener("click", () => {
      closeChoiceMenu(true);
      pick(value);
    });
    panel.append(option);
    return option;
  });
  panel.addEventListener("keydown", event => {
    if (event.key === "Tab" || event.key === "Escape") {
      closeChoiceMenu(true);
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
      }
      return;
    }
    const index = options.indexOf(document.activeElement);
    const next = ({ ArrowDown: (index + 1) % options.length,
      ArrowUp: (index - 1 + options.length) % options.length, Home: 0, End: options.length - 1 })[event.key];
    if (typeof next !== "number") return;
    event.preventDefault();
    event.stopPropagation();
    options[next].focus();
  });

  (document.fullscreenElement || document.body).append(panel);
  const anchor = button.getBoundingClientRect();
  const margin = 4;
  panel.style.width = "max-content";
  panel.style.minWidth = Math.min(Math.max(anchor.width, 134), window.innerWidth - 2 * margin) + "px";
  panel.style.maxWidth = window.innerWidth - 2 * margin + "px";
  panel.style.left = margin + "px";
  panel.style.top = margin + "px";
  if (panel.showPopover) panel.showPopover();
  const width = panel.getBoundingClientRect().width;
  const left = button.dataset.menuAlign === "end" ? anchor.right - width : anchor.left;
  panel.style.left = clamp(left, margin, window.innerWidth - width - margin) + "px";
  const below = window.innerHeight - anchor.bottom - 2 * margin;
  const above = anchor.top - 2 * margin;
  const flip = panel.offsetHeight > below && above > below;
  panel.style.maxHeight = Math.max(0, flip ? above : below) + "px";
  panel.style.top = (flip ? anchor.top - panel.offsetHeight - 2 : anchor.bottom + 2) + "px";

  activeChoice = { button, panel };
  button.setAttribute("aria-expanded", "true");
  button.setAttribute("aria-controls", panel.id);
  (options.find(option => option.getAttribute("aria-selected") === "true") || options[0]).focus({ preventScroll: true });
}

document.addEventListener("pointerdown", event => {
  if (activeChoice && !activeChoice.panel.contains(event.target) && !activeChoice.button.contains(event.target)) {
    closeChoiceMenu();
  }
}, true);
document.addEventListener("focusin", event => {
  if (activeChoice && !activeChoice.panel.contains(event.target) && !activeChoice.button.contains(event.target)) {
    closeChoiceMenu();
  }
});
document.addEventListener("scroll", event => {
  if (activeChoice && event.target !== activeChoice.panel) closeChoiceMenu();
}, true);

function sourcesChanged() {
  commitScenes();
  const old = $("#capture-video");
  if (old) old.currentTime = 0;
  render();
}

function cameraPlacement(camera) {
  const size = clamp((Number(camera.pipSize) || 23) / 100, .1, .45);
  return { size, x: clamp(camera.pipX ?? .74, 0, 1 - size), y: clamp(camera.pipY ?? .66, 0, 1 - size) };
}

function overlayCamera() {
  const sources = activeScene().sources;
  if (sources.some(item => item.type !== "camera" && item.visible))
    return sources.findLast(item => item.type === "camera" && item.visible);
}

function cameraPreviewNotice() {
  const { cameraError, cameraPermissionRestricted } = state.telemetry;
  return UI.text(cameraPermissionRestricted ? "权限受限，请把完整 ZIP 解压到下载目录" :
    cameraError ? "摄像头连接失败" : "正在连接摄像头…");
}

document.addEventListener("pointerdown", event => {
  const outline = event.target.closest(".pip-placement");
  if (!outline || event.button !== 0) return;
  const camera = overlayCamera();
  if (!camera) return;
  const { x, y, size } = cameraPlacement(camera);
  const frame = outline.parentElement.getBoundingClientRect();
  if (!frame.width || !frame.height) return;
  const startX = event.clientX, startY = event.clientY;
  let nextX = x, nextY = y;
  outline.setPointerCapture(event.pointerId);
  const move = pointer => {
    nextX = clamp(x + (pointer.clientX - startX) / frame.width, 0, 1 - size);
    nextY = clamp(y + (pointer.clientY - startY) / frame.height, 0, 1 - size);
    outline.style.left = nextX * 100 + "%";
    outline.style.top = nextY * 100 + "%";
  };
  const finish = pointer => {
    outline.removeEventListener("pointermove", move);
    outline.removeEventListener("pointerup", finish);
    outline.removeEventListener("lostpointercapture", finish);
    if (pointer.type !== "pointerup") {
      outline.style.left = x * 100 + "%";
      outline.style.top = y * 100 + "%";
      return;
    }
    if (nextX === x && nextY === y) return;
    camera.pipX = nextX;
    camera.pipY = nextY;
    const notice = $("#capture-camera-notice", outline.parentElement);
    if (notice) {
      notice.style.left = outline.style.left;
      notice.style.top = outline.style.top;
    }
    commitScenes();
  };
  outline.addEventListener("pointermove", move);
  outline.addEventListener("pointerup", finish);
  outline.addEventListener("lostpointercapture", finish);
});

const wiredPlayers = new WeakSet();

function wirePlayer(kind) {
  const video = $("#" + kind + "-video");
  if (!video) return;
  const playback = state[kind];
  video.muted = playback.volume === 0;
  video.volume = playback.volume / 100;
  video.playbackRate = playback.speed;
  if (!playback.playing) video.pause();
  const current = () => state.page === kind && video.isConnected && $("#" + kind + "-video") === video;
  const updateMetadata = () => {
    if (!current()) return;
    const asset = kind === "editor" ? Views.editorAsset() : Views.currentAsset();
    const width = video.videoWidth || asset.width;
    const height = video.videoHeight || asset.height;
    if (width > 0 && height > 0) video.parentElement.style.setProperty("--media-aspect", width / height);
    if (kind === "review" && asset.id) {
      if (!asset.duration && Number.isFinite(video.duration) && video.duration > 0) asset.duration = video.duration;
      if (!asset.width && video.videoWidth) asset.width = video.videoWidth;
      if (!asset.height && video.videoHeight) asset.height = video.videoHeight;
    }
    if (kind === "review") refreshReviewMetadata();
    if (kind === "editor") {
      applyEditorCropPreview(video.parentElement);
    }
    seekPlayer(kind, kind === "review" ? state.review.at : timeline.at);
  };
  if (wiredPlayers.has(video)) {
    updateMetadata();
    if (playback.playing && video.paused && !timeline.videoClipAt(timeline.at)?.stillId) playMedia(kind, video);
    return;
  }
  wiredPlayers.add(video);
  if (kind === "editor") {
    const asset = Views.editorAsset();
    timeline.frameRate = asset.frameRate || timeline.frameRate;
    observeEditorFrameRate(video, asset.mediaId ? editorProjectAsset(asset.mediaId) : asset);
    updateEditorStillPreview();
  }
  video.addEventListener("loadedmetadata", updateMetadata);
  updateMetadata();
  if (kind === "review") video.addEventListener("timeupdate", () => {
    if (current() && state.review.playing) {
      state.review.at = video.currentTime;
      updateTime("review", state.review.at);
    }
  });
  video.addEventListener("ended", () => {
    if (current() && (kind === "review" || timeline.at >= timeline.duration - .1)) listenPlayer(kind, false);
  });
  video.addEventListener("error", () => { if (current()) handlePlaybackError(kind); });
  if (playback.playing) {
    playMedia(kind, video);
    if (kind === "editor") scheduleEditorMonitor();
  }
}

function refreshReviewMetadata() {
  const asset = Views.currentAsset();
  const range = $("[data-seek='review']");
  if (!range) return;
  const duration = Number.isFinite(asset.duration) && asset.duration > 0 ? asset.duration : 0;
  range.max = duration;
  range.disabled = !duration;
  $("[data-player-duration='review']").textContent = "/ " + time(asset.duration);
  const marks = $(".review-seek-markers", range.parentElement);
  marks.innerHTML = Views.seekMarkers("review", asset.duration, asset.marks);
  $(".encode-data dl").innerHTML = Views.videoInformation(asset);
  updateTime("review", state.review.at);
}

function playMedia(kind, video) {
  video.play().catch(error => {
    if (error.name === "AbortError" || !video.isConnected || state.page !== kind || !state[kind].playing) return;
    if (video.error || error.name === "NotSupportedError") handlePlaybackError(kind);
    else {
      state[kind].playing = false;
      updatePlayButtons(kind);
    }
  });
}

function observeEditorFrameRate(video, asset) {
  if (asset.frameRate || !video.requestVideoFrameCallback) return;
  let previous;
  const intervals = [];
  const observe = (_, frame) => {
    if (!video.isConnected || timeline.media !== asset.id) return;
    const interval = frame.mediaTime - previous;
    previous = frame.mediaTime;
    if (!video.paused && !video.seeking && video.playbackRate <= 1 && interval >= 1 / 240 && interval <= 1 / 6) {
      intervals.push(interval);
      const stable = intervals.length >= 5 && (Math.max(...intervals) - Math.min(...intervals)) < .001;
      if (stable) {
        const estimate = intervals.length / intervals.reduce((sum, value) => sum + value, 0);
        const common = [24000 / 1001, 24, 25, 30000 / 1001, 30, 48, 50, 60000 / 1001, 60, 90, 120, 144];
        const nearest = common.reduce((best, rate) => Math.abs(rate - estimate) < Math.abs(best - estimate) ? rate : best);
        asset.frameRate = Math.abs(nearest - estimate) < estimate * .015 ? nearest : estimate;
        timeline.frameRate = asset.frameRate;
        timeline.setTime(timeline.at);
        return;
      }
      if (intervals.length > 5) intervals.shift();
    } else intervals.length = 0;
    video.requestVideoFrameCallback(observe);
  };
  video.requestVideoFrameCallback(observe);
}

function handlePlaybackError(kind) {
  if (!(kind === "review" ? Views.currentAsset() : Views.editorAsset()).id) return;
  compatibilityPreview(kind);
}

function updatePlayButtons(kind) {
  const active = state[kind].playing;
  const button = $("[data-action='" + kind + "-play'].play-key");
  if (!button) return;
  button.title = active ? "暂停播放" : "播放";
  button.setAttribute("aria-label", button.title);
  button.innerHTML = g(active ? "pause" : "play");
  const center = $("#center-play");
  if (center && kind === "review") center.hidden = active || state.review.at !== 0;
}

function updateTime(kind, at) {
  const range = $("[data-seek='" + kind + "']");
  if (range) {
    range.value = at;
    range.parentElement.style.setProperty("--progress", pct(at, Number(range.max)));
  }
  const clock = $("[data-player-time='" + kind + "']");
  if (clock) clock.textContent = time(at);
  if (kind === "editor") {
    timeline.setTime(at);
    timeline.revealPlayhead();
    const dubStart = $("#dub-start");
    if (dubStart) dubStart.textContent = time(at);
    updateEditorStillPreview();
    renderSubtitleOverlay();
  } else {
    $$(".review-chapter").forEach((row, i, rows) => {
      const next = rows[i + 1]?.dataset.markTime ?? Infinity;
      row.classList.toggle("active", at >= Number(row.dataset.markTime) && at < Number(next));
    });
    updatePlayButtons("review");
  }
}

function seekPlayer(kind, at) {
  if (kind === "editor") {
    state.editor.previewStill = "";
    timeline.setTime(at);
    editorTick = performance.now();
  } else {
    const { duration } = Views.currentAsset();
    state.review.at = duration > 0 ? clamp(at, 0, duration) : Math.max(0, at);
  }
  const target = kind === "editor" ? timeline.videoAt(timeline.at) : state.review.at;
  const video = $("#" + kind + "-video");
  if (video?.readyState >= 1 && Number.isFinite(video.duration)) {
    const position = clamp(target, 0, Math.max(0, video.duration - .001));
    if (Math.abs(video.currentTime - position) > .001) video.currentTime = position;
  }
  updateTime(kind, kind === "editor" ? timeline.at : state.review.at);
}

function listenPlayer(kind, active = !state[kind].playing) {
  const video = $("#" + kind + "-video");
  if (!video) return;
  state[kind].playing = active;
  if (active) {
    if (kind === "editor" && timeline.at >= timeline.duration - .03) seekPlayer(kind, 0);
    if (kind === "review" && state.review.at >= Views.currentAsset().duration - .03) seekPlayer(kind, 0);
    if (kind === "editor") {
      state.editor.previewStill = "";
      updateEditorStillPreview();
      editorTick = performance.now();
    }
    playMedia(kind, video);
    if (kind === "editor") scheduleEditorMonitor();
  } else {
    video.pause();
    if (kind === "editor") stopEditorMonitor();
  }
  updatePlayButtons(kind);
}

function playerVolume(kind, volume, updateInput = false) {
  const video = $("#" + kind + "-video");
  if (!video) return;
  state[kind].volume = volume;
  video.volume = volume / 100;
  video.muted = volume === 0 || (kind === "editor" && !!timeline.videoClipAt(timeline.at)?.stillId);
  if (updateInput) $(`[data-range="${kind}-volume"]`).value = volume;
  const button = $(`[data-action="player-mute"][data-value="${kind}"]`);
  const label = volume ? "静音" : "取消静音";
  button.setAttribute("aria-label", label);
  button.title = label;
  button.innerHTML = g(volume ? "volume" : "mute");
}

function editorDimensions() {
  const asset = Views.editorAsset();
  return [asset.width || 1920, asset.height || 1080];
}

function editorCropAspect() {
  const { crop, cropRatio } = state.editor;
  if (!crop || crop === "自由") return 0;
  if (crop === "自定义") return cropRatio.width / cropRatio.height;
  const [w, h] = crop.split(":").map(Number);
  return w / h;
}

function currentEditorCrop() {
  const [w, h] = editorDimensions();
  if (state.editor.cropRect) return state.editor.cropRect;
  const ratio = editorCropAspect();
  if (!ratio) return [0, 0, w, h];
  const width = Math.min(w, h * ratio);
  const height = Math.min(h, w / ratio);
  return [(w - width) / 2, (h - height) / 2, width, height];
}

function editorCropGeometry(rect = currentEditorCrop()) {
  const [w, h] = editorDimensions();
  const [x, y, width, height] = rect;
  return `left:${pct(x, w)};top:${pct(y, h)};width:${pct(width, w)};height:${pct(height, h)}`;
}

function applyEditorCropPreview(frame, rect = currentEditorCrop()) {
  const [w, h] = editorDimensions();
  const [x, y, width, height] = rect;
  if (!(width > 0 && height > 0)) return;
  const editing = Boolean(state.editor.crop);
  frame.dataset.cropped = String(!editing && (x > 0 || y > 0 || width < w || height < h));
  frame.style.setProperty("--media-aspect", editing ? w / h : width / height);
  frame.style.setProperty("--source-x", `${-x * 100 / width}%`);
  frame.style.setProperty("--source-y", `${-y * 100 / height}%`);
  frame.style.setProperty("--source-width", `${w * 100 / width}%`);
  frame.style.setProperty("--source-height", `${h * 100 / height}%`);
  const overlay = $("#editor-subtitles", frame);
  if (overlay) {
    overlay.style.inset = editing ? `${y * 100 / h}% auto auto ${x * 100 / w}%` : "";
    overlay.style.width = editing ? `${width * 100 / w}%` : "";
    overlay.style.height = editing ? `${height * 100 / h}%` : "";
  }
  const outline = $(".crop-outline", frame);
  if (outline) outline.style.cssText = editorCropGeometry(rect);
}

function dragEditorCrop(event, outline) {
  if (event.button !== 0) return;
  const corner = event.target.dataset.cropCorner;
  const [w, h] = editorDimensions();
  const [x, y, width, height] = currentEditorCrop();
  const { width: screenW, height: screenH } = outline.parentElement.getBoundingClientRect();
  if (!screenW || !screenH) return;
  const ratio = editorCropAspect();
  const fromWest = corner?.includes("w");
  const fromNorth = corner?.includes("n");
  const anchorX = fromWest ? x + width : x;
  const anchorY = fromNorth ? y + height : y;
  const editor = state.editor;
  const pointerId = event.pointerId;
  const start = [x, y, width, height];
  let next = start;
  event.preventDefault();
  outline.setPointerCapture(pointerId);
  const move = pointer => {
    if (pointer.pointerId !== pointerId || !outline.isConnected) return;
    const dx = (pointer.clientX - event.clientX) * w / screenW;
    const dy = (pointer.clientY - event.clientY) * h / screenH;
    if (!corner) next = [clamp(x + dx, 0, w - width), clamp(y + dy, 0, h - height), width, height];
    else {
      const maxW = fromWest ? anchorX : w - anchorX;
      const maxH = fromNorth ? anchorY : h - anchorY;
      let newW = width + (fromWest ? -dx : dx);
      let newH = height + (fromNorth ? -dy : dy);
      if (ratio) {
        const limit = Math.min(maxW, maxH * ratio);
        newW = Math.abs(dx / width) > Math.abs(dy / height) ? newW : newH * ratio;
        newW = clamp(newW, Math.min(limit, Math.max(2, 2 * ratio)), limit);
        newH = newW / ratio;
      } else {
        newW = clamp(newW, Math.min(16, maxW), maxW);
        newH = clamp(newH, Math.min(16, maxH), maxH);
      }
      next = [fromWest ? anchorX - newW : anchorX, fromNorth ? anchorY - newH : anchorY, newW, newH];
    }
    applyEditorCropPreview(outline.parentElement, next);
  };
  const finish = pointer => {
    if (pointer.pointerId !== pointerId) return;
    document.removeEventListener("pointermove", move, true);
    document.removeEventListener("pointerup", finish, true);
    document.removeEventListener("pointercancel", finish, true);
    if (editor !== state.editor || !outline.isConnected) return;
    if (pointer.type === "pointerup" && next !== start) {
      editor.cropRect = next;
      queueEditorAutosave();
    } else applyEditorCropPreview(outline.parentElement, start);
  };
  document.addEventListener("pointermove", move, true);
  document.addEventListener("pointerup", finish, true);
  document.addEventListener("pointercancel", finish, true);
}

document.addEventListener("pointerdown", event => {
  const outline = event.target.closest(".crop-outline");
  if (outline) dragEditorCrop(event, outline);
});

function openCustomCrop() {
  const { width, height } = state.editor.cropRatio;
  showDialog("editor-crop", "自定义取景", `<form id="editor-crop-form" class="form" data-form="editor-crop">
    <label for="crop-width">宽度</label><input type="number" id="crop-width" name="width" min="1" max="8192" step="1" value="${width}" required autofocus>
    <label for="crop-height">高度</label><input type="number" id="crop-height" name="height" min="1" max="8192" step="1" value="${height}" required>
    </form>`, `<button type="submit" form="editor-crop-form" class="btn primary">确定</button>`);
}

function editorStills() {
  const samples = [
    { id: "builtin-alpine", title: "高山湖", image: "assets/alpine-lake.png" },
    { id: "builtin-study", title: "黄昏书房", image: "assets/study-at-dusk.png" },
    { id: "builtin-intermission", title: "休息画面", image: "assets/intermission.png" },
  ];
  return [...state.editor.stills, ...samples];
}

function editorStillItem(id) {
  if (!id) return undefined;
  return editorStills().find(still => still.id === id);
}

function updateEditorStillPreview() {
  const preview = $("#editor-preview-still");
  if (!preview) return;
  const stillId = state.editor.previewStill || timeline.videoClipAt(timeline.at)?.stillId;
  const still = editorStillItem(stillId);
  if (still?.image && preview.getAttribute("src") !== still.image) preview.src = still.image;
  preview.hidden = !still?.image;
  $("[data-action='still-close']")?.classList.toggle("visible", !!state.editor.previewStill);
  const video = $("#editor-video");
  if (video) video.muted = state.editor.volume === 0 || !!still || hasEditorMonitorAudio();
}

async function saveEditorStill() {
  if (!hasExplicitEditorProject()) { toast("请先打开工程。"); return; }
  const still = editorStillItem(state.editor.previewStill || timeline.videoClipAt(timeline.at)?.stillId);
  if (still && state.editor.stills.some(item => item.id === still.id)) return;
  if (still) {
    state.editor.stills.unshift(still);
    timelineChange();
    return;
  }
  const video = $("#editor-video");
  if (!video?.videoWidth) { toast("画面尚未加载。"); return; }
  const canvas = document.createElement("canvas");
  const scale = Math.min(1, 1920 / video.videoWidth);
  canvas.width = Math.round(video.videoWidth * scale);
  canvas.height = Math.round(video.videoHeight * scale);
  canvas.getContext("2d").drawImage(video, 0, 0, canvas.width, canvas.height);
  const editor = state.editor;
  const at = timeline.at;
  try {
    const file = await Native.rpc("save-still", { data: canvas.toDataURL("image/jpeg", .88) });
    if (editor !== state.editor) return;
    editor.stills.unshift({ id: file.id, title: "静止画面 " + (editor.stills.length + 1), at, image: file.url });
    timelineChange();
  } catch (error) { reportNativeError(error); }
}

async function importEditorStills(input) {
  const target = state.editor.stills;
  const files = [...input.files];
  input.value = "";
  let skipped = false;
  for (const file of files) {
    try {
      const bitmap = await createImageBitmap(file);
      bitmap.close();
    } catch {
      skipped = true;
      continue;
    }
    const registered = await registerEditorStillFile(file);
    if (!registered) continue;
    if (state.editor.stills !== target) return;
    target.unshift({ ...registered, title: file.name.replace(/\.[^.]+$/, "") });
  }
  timelineChange();
  queueEditorAutosave();
  if (skipped) toast("部分图片无法读取。");
}

function openStillInsert(id) {
  const still = editorStillItem(id);
  if (!still) return;
  if (timeline.tracks[0].locked) { toast("主视频轨已锁定。"); return; }
  showDialog("editor-still", "插入静止画面", `<form id="editor-still-form" class="form" data-form="editor-still" data-still="${esc(id)}">
    <label for="still-length">时长</label><div class="hstack"><input type="number" id="still-length" name="length" value="3" min=".1" max="600" step=".1" required autofocus><small>秒</small></div>
    <label for="still-caption">字幕</label><input type="text" id="still-caption" name="caption" maxlength="120">
    </form>`, `<button type="submit" form="editor-still-form" class="btn primary">插入</button>`);
}

function stepChapter(kind, step) {
  const marks = kind === "review" ? Views.currentAsset().marks : timeline.markers;
  const nowAt = kind === "review" ? state.review.at : timeline.at;
  const mark = step === -1 ? marks.findLast(item => item.at < nowAt - .3) : marks.find(item => item.at > nowAt + .3);
  seekPlayer(kind, mark?.at ?? (step < 0 ? 0 : kind === "review" ? Views.currentAsset().duration : timeline.duration));
}

function subtitleStyleFor(track, clip) {
  const { subtitleFont, subtitleSize, subtitleColor, subtitleOpacity } = state.editor;
  const alpha = Math.round(clamp(subtitleOpacity, 0, 100) * 255 / 100).toString(16).padStart(2, "0");
  return clip?.style || track?.style || { font: subtitleFont, pixels: subtitleSize, color: "#" + alpha + subtitleColor.slice(1) };
}

function captionColor(argb) {
  return /^#[\da-f]{8}$/i.test(argb) ? argb.slice(0, 1) + argb.slice(3) + argb.slice(1, 3) : "#ffffffff";
}

function openSubtitleStyle(target) {
  if (!hasExplicitEditorProject()) { toast("请先打开剪辑工程。"); return; }
  const [scope, id] = target.split(":");
  const isClip = scope === "clip";
  const track = timeline.tracks.find(track => track.kind === "subtitle" &&
    (isClip ? track.clips.some(clip => clip.id === id) : track.id === id));
  if (!track || track.locked) { toast("请先选择并解锁字幕轨。"); return; }
  const clip = isClip ? track.clips.find(clip => clip.id === id) : undefined;
  const item = clip || track;
  const inherited = clip ? "跟随字幕轨" : "跟随默认样式";
  const label = name => esc(UI.text(name));
  showDialog("subtitle-style", clip ? "字幕片段样式" : "字幕轨样式",
    `<form id="subtitle-style-form" class="caption-style-form">
      <div class="caption-style-preview" aria-hidden="true"><span>${esc(clip?.name || clip?.translation || UI.text("字幕预览"))}</span></div>
      <div class="caption-style-field">
        <label for="caption-font">${label("字体")}</label>
        <select id="caption-font" name="font" required></select>
      </div>
      <div class="caption-style-controls">
        <div class="caption-style-field">
          <label for="caption-pixels">${label("字号")}</label>
          <div class="caption-style-number">
            <input id="caption-pixels" name="pixels" type="number" min="0" max="320" step="1" title="${label("0 为自动字号")}" required>
            <small id="caption-pixels-unit">px</small>
          </div>
        </div>
        <div class="caption-style-field">
          <label for="caption-color">${label("颜色")}</label>
          <input id="caption-color" name="color" type="color">
        </div>
        <div class="caption-style-field">
          <label for="caption-opacity">${label("不透明度")}</label>
          <div class="caption-style-number">
            <input id="caption-opacity" name="opacity" type="number" min="0" max="100" step=".01" required>
            <small>%</small>
          </div>
        </div>
      </div>
    </form>`,
    `<div class="hstack caption-style-actions">
       <button type="button" class="btn flat" id="import-caption-style">${label("导入样式…")}</button>
       <button type="button" class="btn flat" id="reset-caption-style"${item.style ? "" : " disabled"}>${label(inherited)}</button>
     </div>
     <div class="hstack">
       ${b("close-dialog", "取消", null)}
       <button type="submit" class="btn primary" form="subtitle-style-form">${label("应用")}</button>
     </div>`, { cancel: false });
  const form = $("#subtitle-style-form", dialog);
  const preview = $(".caption-style-preview span", form);
  const pixelsUnit = $("#caption-pixels-unit", form);
  let families = [];
  const setFont = selected => {
    const choices = families.includes(selected) ? families : [selected, ...families];
    form.elements.font.replaceChildren(...choices.map(family => new Option(family, family, false, family === selected)));
  };
  const updatePreview = () => {
    const pixels = Number(form.elements.pixels.value);
    pixelsUnit.textContent = pixels ? "px" : UI.text("自动");
    preview.style.fontFamily = JSON.stringify(form.elements.font.value) + ", sans-serif";
    preview.style.fontSize = clamp(pixels || 24, 8, 50) + "px";
    preview.style.color = form.elements.color.value;
    preview.style.opacity = clamp(Number(form.elements.opacity.value) / 100, 0, 1);
  };
  const fill = style => {
    const color = captionColor(style.color);
    setFont(style.font);
    form.elements.pixels.value = style.pixels;
    form.elements.color.value = color.slice(0, 7);
    form.elements.opacity.value = Number((parseInt(color.slice(-2), 16) * 100 / 255).toFixed(2));
    updatePreview();
  };
  fill(subtitleStyleFor(track, clip));
  form.addEventListener("input", updatePreview);
  Native.rpc("subtitle-style-fonts").then(installed => {
    if (!form.isConnected || !dialog.open || !Array.isArray(installed)) return;
    const collator = new Intl.Collator(document.documentElement.lang, { numeric: true });
    families = installed.sort(collator.compare);
    setFont(form.elements.font.value);
  }).catch(error => {
    if (form.isConnected && dialog.open) reportNativeError(error);
  });
  $("#import-caption-style", dialog).addEventListener("click", async () => {
    try {
      const style = await Native.rpc("subtitle-style-import");
      if (style && form.isConnected && dialog.open) fill(style);
    } catch (error) { reportNativeError(error); }
  });
  const apply = style => {
    if (!timeline.tracks.includes(track) || (clip && !track.clips.includes(clip))) return;
    timeline.remember();
    if (style) item.style = style;
    else delete item.style;
    dialog.close();
    timelineChange();
  };
  $("#reset-caption-style", dialog).addEventListener("click", () => apply());
  form.addEventListener("submit", event => {
    event.preventDefault();
    const fields = new FormData(form);
    const font = String(fields.get("font")).trim();
    const pixels = Number(fields.get("pixels"));
    if (!font || /[,\x00-\x1f]/.test(font) || (pixels > 0 && pixels < 8)) {
      toast("请输入有效字体，字号为 0 或 8–320。");
      return;
    }
    const opacity = Math.round(Number(fields.get("opacity")) * 255 / 100).toString(16).padStart(2, "0");
    apply({ font, pixels, color: "#" + opacity + fields.get("color").slice(1) });
  });
}

function renderSubtitleOverlay() {
  const overlay = $("#editor-subtitles");
  if (!overlay) return;
  const sourceHeight = state.editor.crop ? editorDimensions()[1] : currentEditorCrop()[3];
  const cues = timeline.tracks.filter(track => track.kind === "subtitle" && !track.muted).map((track, i) => {
    const clip = track.clips.find(item => item.at <= timeline.at && timeline.at < item.at + item.length);
    return clip ? { track, clip, i } : null;
  }).filter(Boolean);
  const styles = cues.map(({ track, clip }) => subtitleStyleFor(track, clip));
  const html = cues.map(({ track, clip, i }) => `<span class="positioned-caption" data-caption-track="${esc(track.id)}"
    style="left:${100 * (track.x ?? .5)}%;top:${100 * (track.y ?? (.9375 - .07 * i))}%">${esc(clip.name || clip.translation)}</span>`).join("");
  const signature = JSON.stringify([html, styles, sourceHeight]);
  if (overlay.renderedCues !== signature && !captionDrag) {
    overlay.innerHTML = html;
    Array.from(overlay.children).forEach((caption, i) => {
      const style = styles[i];
      const pixels = style.pixels || clamp(Math.round(sourceHeight / 20), 14, 80);
      const color = captionColor(style.color);
      caption.style.color = color.slice(0, 7);
      caption.style.opacity = parseInt(color.slice(-2), 16) / 255;
      caption.style.fontFamily = JSON.stringify(style.font);
      caption.style.fontSize = `${(pixels * 100 / Math.max(1, sourceHeight)).toFixed(3)}cqh`;
    });
    overlay.renderedCues = signature;
  }
}

function timelineChange({ timelineOnly = false } = {}) {
  if (state.page !== "editor") return;
  queueEditorAutosave();
  if (timelineOnly) {
    timeline.render();
    return;
  }
  timeline.render();
  if (timeline.selectedTrack?.kind === "subtitle") state.editor.subtitleTrack = timeline.selectedTrack.id;
  const options = $(".editor-options");
  if (options) {
    const template = document.createElement("template");
    const tabs = $(".panel-head", options);
    if ($("[aria-pressed='true']", tabs)?.dataset.value !== state.editor.tab) tabs.outerHTML = Views.editorOptionTabs();
    $(".scroll-body", options).innerHTML = Views.editorOptionBody();
    template.innerHTML = Views.editorBin();
    $(".editor-bin").innerHTML = template.content.firstElementChild.innerHTML;
    $("[data-seek='editor']").parentElement.outerHTML = Views.seeker("editor", timeline.at, timeline.duration, timeline.markers);
    $("[data-player-duration='editor']").textContent = "/ " + time(timeline.duration);
  }
  seekPlayer("editor", timeline.at);
}

function openEditor(asset = Views.currentAsset(), at) {
  switchEditorAsset(asset, at);
}

function chooseReview(id) {
  state.asset = id;
  state.review.at = 0;
  state.review.playing = false;
  navigate("review");
}

function saveReviewMarks(asset) {
  Native.rpc("marks", { id: asset.id, marks: asset.marks }).catch(reportNativeError);
}

function addReviewMark() {
  const asset = Views.currentAsset();
  if (!asset.id) return;
  const video = $("#review-video");
  const at = clamp(state.review.playing && video ? video.currentTime : state.review.at, 0, asset.duration);
  const existing = asset.marks.find(mark => Math.abs(mark.at - at) < .25);
  if (existing) {
    editReviewMark(existing.at);
    return;
  }
  asset.marks.push({ at, name: `打点 ${asset.marks.length + 1}` });
  asset.marks.sort((a, b) => a.at - b.at);
  saveReviewMarks(asset);
  $(".review-info .chapter-list").innerHTML = Views.chapters(asset.marks, "review");
  $(".review-seek-markers").innerHTML = Views.seekMarkers("review", asset.duration, asset.marks);
  $("#review-mark-count").textContent = `(${asset.marks.length})`;
  $(".library-list").innerHTML = Views.libraryItems();
  updateTime("review", state.review.at);
  editReviewMark(at);
}

function editReviewMark(value) {
  const at = Number(value);
  const asset = Views.currentAsset();
  const mark = asset.marks.find(item => item.at === at);
  const row = $(`.review-chapter[data-mark-time="${at}"]`);
  if (!row || !mark) return;
  const button = $(".chapter-name", row);
  if (!button) {
    $(".chapter-name-input", row)?.focus();
    return;
  }
  const input = document.createElement("input");
  input.type = "text";
  input.className = "chapter-name-input";
  input.setAttribute("aria-label", "打点名称");
  input.maxLength = 42;
  input.value = mark.name;
  const finish = (commit, focus) => {
    input.removeEventListener("blur", onBlur);
    const name = input.value.trim();
    if (commit && name && name !== mark.name) {
      mark.name = name;
      $(".truncate", button).textContent = name;
      button.title = name;
      button.setAttribute("aria-label", "编辑打点 " + name);
      const dot = $(`.review-seek-markers .seek-marker[data-value="${at}"]`);
      if (dot) {
        dot.title = name + " · " + time(at);
        dot.setAttribute("aria-label", name);
      }
      saveReviewMarks(asset);
    }
    input.replaceWith(button);
    if (focus) button.focus({ preventScroll: true });
  };
  const onBlur = () => finish(true, false);
  input.addEventListener("blur", onBlur);
  input.addEventListener("keydown", event => {
    if (event.isComposing || !["Enter", "Escape"].includes(event.key)) return;
    event.preventDefault();
    event.stopPropagation();
    finish(event.key === "Enter", true);
  });
  button.replaceWith(input);
  input.focus();
  input.select();
}

function mixerTrackModes() {
  const tracks = Object.keys(state.audio).length;
  return [["双独立音轨", tracks === 2 ? "双独立音轨" : "独立音轨"], "混合音轨", "无音频"];
}

function availableMixerSources() {
  return Object.keys(mixerSources).filter(id => mixerSources[id].optional && !Object.hasOwn(state.audio, id));
}

function addMixerSourceDialog() {
  const available = availableMixerSources();
  if (!available.length) return;
  const choices = available.map((id, i) => {
    const { name, icon } = mixerSources[id];
    return `<label class="mixer-source-choice">
      <input type="radio" name="mixer-source" value="${id}"${i ? "" : " checked autofocus"}>
      ${g(icon)}<span>${esc(name)}</span>
    </label>`;
  }).join("");
  showDialog("mixer-add", "添加音频来源", `<div class="mixer-source-list stack">${choices}</div>`,
    b("mixer-add", "添加", null, { variant: "primary" }));
}

function renderMixer({ reveal = false } = {}) {
  const dock = $(".mixer-dock");
  if (!dock) return;
  const scroll = $(".scroll-body", dock).scrollTop;
  const template = document.createElement("template");
  template.innerHTML = Views.mixerDock();
  const replacement = template.content.firstElementChild;
  dock.replaceWith(replacement);
  const body = $(".scroll-body", replacement);
  body.scrollTop = reveal ? body.scrollHeight : scroll;
}

function renderRecordState() {
  const dock = $(".control-dock");
  if (dock) {
    const template = document.createElement("template");
    template.innerHTML = Views.controlDock();
    dock.replaceWith(template.content.firstElementChild);
  }
  chrome();
}

function recordAction() {
  Native.rpc("record").catch(reportNativeError);
}

function confirmDialog(title, message, action, label = "确定") {
  showDialog("confirm", title, `<p>${esc(message)}</p>`, b(action, label, "check", { variant: "primary" }));
}

function showDialog(name, title, body, footer = "", { cancel = true } = {}) {
  closeChoiceMenu();
  dialogName = name;
  dialog.dataset.kind = name;
  const caption = UI.text(title);
  dialog.innerHTML = `<div class="panel-head"><h2 class="section-title">${esc(caption)}</h2>${ib("close-dialog", "关闭窗口", "close")}</div>
    <div class="dialog-body stack">${body}</div>${footer ? `<div class="panel-foot">${cancel ? b("close-dialog", "取消", null) : ""}${footer}</div>` : ""}`;
  dialog.setAttribute("aria-label", caption);
  if (!dialog.open) dialog.showModal();
}

function dateInput(at) {
  const date = new Date(at);
  return new Date(at - date.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
}

function scheduleDialog() {
  const tasks = state.schedule.map((task, i) => ({ ...task, i })).sort((a, b) => a.at - b.at);
  const currentTime = Date.now();
  const intervalFields = [["hours", "hh", "小时"], ["minutes", "mm", "分钟"], ["seconds", "ss", "秒"]]
    .map(([name, placeholder, label]) => `<input type="number" name="${name}" min="0" step="1" ${name === "hours" ? "" : 'max="59"'}
    inputmode="numeric" placeholder="${placeholder}" aria-label="自动停录${label}">`).join('<span aria-hidden="true">:</span>');
  showDialog("schedule", "预约录制", `<div class="field-group inset">
    <form class="schedule-form" data-form="schedule">
    <label>${tr("名称")}<input type="text" name="name" required></label>
    <label>${tr("开始时间")}<input type="datetime-local" name="at" required value="${dateInput(currentTime + 7200e3)}"></label>
    <fieldset class="schedule-duration"><legend>${tr("自动停录时长")} <small>${tr("可选")}</small></legend>
      <div class="schedule-interval">${intervalFields}</div>
    </fieldset>
    <label>${tr("场景")}<select name="scene">${state.scenes.map(scene => `<option value="${esc(scene.name)}">${tr(scene.name)}</option>`).join("")}</select></label>
    <button class="btn primary form-action" type="submit">${g("plus")}${tr("添加预约")}</button></form></div>
    <strong>${tr("预约队列")}</strong>${tasks.map(task => {
      const expired = task.status === "expired";
      const overlaps = task.status === "conflict";
      const stopMode = task.duration ? `${tr("自动停录")} ${time(task.duration, true)}` : tr("手动停录");
      return `<div class="schedule-item${overlaps ? " warning" : ""}"><div class="hstack">
        <strong>${esc(task.name)}</strong><span class="tag ${overlaps ? "warning" : expired ? "" : "waiting"}">
          ${tr(({ conflict: "时段冲突", expired: "已过期", waiting: "等待触发", recording: "录制中", completed: "已完成", missed: "已错过", failed: "失败" })[task.status] || task.status)}</span>
        <span class="grow"></span>${ib("remove-schedule", "删除预约", "trash", { value: task.i })}</div>
        <small class="mono">${dateInput(task.at).replace("T", " ")} · ${stopMode} · ${tr(task.scene)}</small></div>`;
    }).join("")}`);
}

function updateOutputFolder(name) {
  state.settings.outputPath = name;
  state.settings.outputPathIsFolderName = false;
  $$("[data-output-path]").forEach(input => {
    input.value = name;
    input.title = `已选目录 ${name}（浏览器只提供名称）`;
  });
}

async function chooseDirectory(id, onChosen) {
  if (typeof window.showDirectoryPicker === "function") {
    try {
      const folder = await window.showDirectoryPicker({ id, mode: "read" });
      onChosen(folder.name);
      return;
    } catch (error) {
      if (error.name === "AbortError") return;
    }
  }
  const input = document.createElement("input");
  input.type = "file";
  input.webkitdirectory = true;
  input.addEventListener("change", () => {
    const path = input.files[0]?.webkitRelativePath;
    const name = path?.split("/")[0];
    if (name) onChosen(name);
  }, { once: true });
  input.click();
}

function chooseOutputPath() {
  return chooseDirectory("recorder-output", updateOutputFolder);
}

function settingsDialog(tab = "general", model = "") {
  const categories = [["general", "常规", "cog"], ["theme", "主题设置", "palette"], ["audio", "音频", "wave"], ["shortcuts", "系统快捷键", "link"],
    ["models", "模型服务", "chip"], ["conversations", "会话管理", "folder"], ["system", "系统信息", "book"]];
  const s = state.settings;
  const label = name => esc(UI.text(name));
  let content;
  if (tab === "theme") {
    content = ThemeManager.renderSettings();
  } else if (tab === "conversations") {
    content = AgentWorkspace.archivedSettings();
  } else if (tab === "models" || tab === "shortcuts") {
    content = "";
  } else if (tab === "system") {
    content = `<dl class="system-settings-list">
      <dt>${label("当前版本")}</dt><dd>${esc(state.version || "—")}</dd>
      <dt>${label("本地配音")}</dt><dd>Audio8 0.1B · ${label(state.modelReady ? "可用" : "未安装")}</dd>
      <dt>${label("上下文压缩")}</dt><dd>${label("使用助手模型自动压缩")}</dd>
      </dl>`;
  } else if (tab === "audio") {
    content = `<div class="form">${Views.formSelect("audio-mode", "录音轨道模式", mixerTrackModes(), s.audioMode)}
      </div>
      ${Views.checkbox("denoise", "开启麦克风降噪", s.denoise)}`;
  } else {
    content = `<div class="form">${Views.formSelect("language", "界面语言", [["zh-CN", "简体中文"], ["en", "English"]], s.language)}
      ${Views.formSelect("countdown", "录制倒计时", countdownOptions, s.countdown)}
      ${Views.outputPath("general-output-path")}
      </div>`;
  }
  const side = categories.map(([value, name, icon]) => b("settings", name, icon, {
    value, variant: "flat", pressed: value === tab,
  })).join("");
  SettingsEditors.leave();
  showDialog("settings", "设置", `<div class="settings-layout">
    <nav class="settings-nav" aria-label="${label("设置分类")}">${side}</nav>
    <section class="settings-content" aria-label="${label(categories.find(row => row[0] === tab)?.[1] || "常规")}">${content}</section></div>`,
    b("close-dialog", "完成", null, { variant: "primary" }), { cancel: false });
  if (tab === "models") SettingsEditors.models($(".settings-content", dialog), model);
  if (tab === "shortcuts") SettingsEditors.hotkeys($(".settings-content", dialog));
  $(".settings-nav [aria-pressed='true']", dialog)?.focus({ preventScroll: true });
}

function openExport(mode) {
  const project = state.page === "editor";
  const clip = project ? timeline.selectedClip : null;
  if (mode === "clip" && !clip) { toast("请先选择要导出的片段。"); return; }
  const asset = project ? Views.editorAsset() : Views.currentAsset();
  if (!asset.id) { toast("请先选择视频。"); return; }
  exportDraft = {
    project, asset, clip, scope: mode === "clip" ? "clip" : "all",
    format: state.settings.format, encoder: "auto", audio: "separate", subtitles: "burn",
    height: "0", bitrate: clamp(Math.round(asset.bitrate || state.settings.bitrate || 6000), 100, 100000), audioBitrate: "192",
    copyStreams: false, denoise: false, subtitleFile: "",
    name: asset.name.replace(/\.[^.]+$/, "") + (project ? "-成片" : "-导出"),
    folder: state.settings.outputPath,
  };
  exportDialog();
}

function exportDialog(focusField) {
  const draft = exportDraft;
  const { project, asset, clip, scope, format, copyStreams, subtitleFile } = draft;
  const partial = project && scope === "clip";
  const duration = project ? (partial ? clip.length : timeline.duration) : asset.duration;
  const summary = partial ? time(clip.at) + "–" + time(clip.at + clip.length) : time(duration);
  const containers = ["MP4", "MOV", "MKV"];
  const formats = project ? containers : [...containers, "WebM", "AVI", "FLV"];
  const compatible = containers.includes(format);
  const gifTooLong = format === "GIF" && duration > 60;
  const field = (name, label, choices, disabled = false) =>
    `<label for="export-${name}">${tr(label)}</label>${select("export-" + name, label, choices, draft[name],
      `id="export-${name}" name="${name}"${disabled ? " disabled" : ""}`)}`;
  const check = (name, label, disabled = false) => `<label class="hstack export-wide">
    <input name="${name}" type="checkbox"${draft[name] ? " checked" : ""}${disabled ? " disabled" : ""}> ${tr(label)}</label>`;
  const scopeField = clip ? field("scope", "范围", [["all", "完整成片"], ["clip", "选中片段"]]) : "";
  const projectOptions = project ? field("height", "分辨率", [["0", "原始分辨率"], ["720", "最高 720p"], ["1080", "最高 1080p"],
    ["1440", "最高 1440p"], ["2160", "最高 2160p"]]) +
    `<label for="export-bitrate">${tr("视频码率")}</label><div class="form-row">
      <input id="export-bitrate" type="number" min="100" max="100000" step="1" name="bitrate" value="${draft.bitrate}" required><small>kbps</small></div>` +
    field("audioBitrate", "音频码率", [96, 128, 192, 256, 320].map(rate => [rate, rate + " kbps"])) +
    field("audio", "音轨", [["separate", "保留独立音轨"], ["mix", "混合成一轨"]]) +
    (partial ? `<label>${tr("字幕")}</label><span>${tr("可选字幕轨")}</span>` :
      field("subtitles", "字幕", [["burn", "写入画面"], ["track", "可选字幕轨"]])) : "";
  const sourceOptions = !project ? `${compatible || format === "WebM" ? `<label for="export-srt">${tr("字幕文件")}</label>
    <div class="form-row export-srt">
      <input id="export-srt" type="file" name="subtitleFile" accept=".srt" hidden>
      ${b("export-select-srt", "选择 SRT…", "folder", { variant: "mini", disabled: copyStreams })}
      ${subtitleFile ? `<small class="truncate" title="${esc(subtitleFile)}">${esc(subtitleFile)}</small>` : ""}
      ${subtitleFile ? b("export-clear-srt", "清除字幕", "close", { iconOnly: true, variant: "flat mini" }) : ""}
    </div>` : ""}
    ${format !== "GIF" ? check("denoise", "降噪与音量整理", copyStreams) : ""}
    ${compatible ? check("copyStreams", "快速转封装", !!subtitleFile || draft.denoise) : ""}` : "";
  showDialog("export", project ? "导出剪辑成片" : "导出／格式转换", `<form class="form export-form" id="export-form">
    <label>${tr("来源")}</label><div class="hstack spaced"><strong class="truncate" title="${esc(asset.name)}">${esc(asset.title)}</strong>
      <time class="soft">${summary}</time></div>
    ${scopeField}${field("format", "格式", formats)}
    ${compatible ? field("encoder", "编码", [["auto", "自动 · 优先 GPU"], ["libx264", "CPU · x264"], ["h264_nvenc", "NVIDIA · NVENC"]], copyStreams) : ""}
    ${projectOptions}${sourceOptions}
    <label for="export-name">${tr("文件名")}</label><div class="form-row"><input id="export-name" name="name" type="text"
      value="${esc(draft.name)}" required maxlength="100" autocomplete="off"><small class="mono">.${format.toLowerCase()}</small></div>
    <label for="export-folder">${tr("路径")}</label><div class="form-row"><input id="export-folder" name="folder" type="text"
      value="${esc(draft.folder)}" title="${esc(draft.folder)}" readonly>${b("export-path", "浏览…", null, { variant: "mini" })}</div>
    ${gifTooLong ? `<small class="export-wide export-warning" role="status">${tr("GIF 最长 60 秒")}</small>` : ""}
    </form>`, `<button type="submit" form="export-form"
      class="btn primary"${gifTooLong ? " disabled" : ""}>${g("download")}${tr("开始导出")}</button>`);
  const form = $("#export-form", dialog);
  form.addEventListener("input", event => {
    if (event.target.name === "name") {
      draft.name = event.target.value;
      event.target.setCustomValidity("");
    }
  });
  form.addEventListener("change", event => {
    const { name, type, value, checked, files } = event.target;
    if (name === "name" || !Object.hasOwn(draft, name)) return;
    draft[name] = type === "checkbox" ? checked : type === "file" ? files[0]?.name || "" : value;
    if (!containers.includes(draft.format)) draft.copyStreams = false;
    if (![...containers, "WebM"].includes(draft.format)) draft.subtitleFile = "";
    if (draft.format === "GIF") draft.denoise = false;
    if (["scope", "format", "copyStreams", "denoise", "subtitleFile"].includes(name)) exportDialog(name);
  });
  form.addEventListener("submit", event => {
    event.preventDefault();
    const name = draft.name.trim();
    if (!name || /[<>:"/\\|?*\u0000-\u001f]/.test(name) || /[. ]$/.test(name)) {
      const input = form.elements.name;
      input.setCustomValidity("请输入有效的文件名。");
      input.reportValidity();
      return;
    }
    const button = $("[form='export-form']", dialog);
    button.disabled = true;
    button.textContent = "导出中…";
    (project ? flushEditorChanges() : Promise.resolve()).then(() => Native.rpc("export", {
      id: asset.id, kind: project ? "editor" : "review", name, folder: draft.folder, format, encoder: draft.encoder,
      selected: partial, start: partial ? clip.at : 0, length: duration, mix: draft.audio === "mix",
      height: Number(draft.height), bitrate: Number(draft.bitrate), audioBitrate: Number(draft.audioBitrate),
      burn: !partial && draft.subtitles === "burn", copy: draft.copyStreams, denoise: draft.denoise,
      subtitle: draft.subtitleId,
    })).then(path => { if (dialog.open) dialog.close(); toast("已导出 " + path); }, error => {
      reportNativeError(error);
      if (button.isConnected) { button.disabled = false; button.textContent = "开始导出"; }
    });

  });
  if (focusField) {
    const control = focusField === "subtitleFile" ? $("[data-action='export-select-srt']", form) : form.elements.namedItem(focusField);
    control?.focus({ preventScroll: true });
  }
}

function saveLink(url, name) {
  const link = document.createElement("a");
  link.href = url;
  link.download = name;
  link.click();
}

function downloadJson(data, name) {
  const url = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2) + "\n"], { type: "application/json" }));
  saveLink(url, name);
  setTimeout(() => URL.revokeObjectURL(url), 1500);
}

function saveProject() {
  const asset = Views.editorAsset();
  const saved = persistEditorDraft({ explicit: true });
  if (!saved) return;
  downloadJson({ schema: editorProjectSchema, source: asset.name, sourceId: saved.mediaId, title: saved.title, edit: saved.edit,
    timeline: saved.timeline, settings: saved.editor },
    asset.name.replace(/\.[^.]+$/, "") + ".wpsrecorder-mock.json");
  toast("已下载原型剪辑工程。");
}

dialog.addEventListener("close", () => {
  if (dialog.open) return;
  SettingsEditors.leave();
  if (dialogName === "settings" && state.page === "record") render();
});

function locateSubtitleTrack() {
  return timeline.tracks.find(track => track.id === state.editor.subtitleTrack);
}

function addSubtitle(name, start = timeline.at, length = 3) {
  const track = locateSubtitleTrack();
  if (!track || track.locked) { toast("请先解锁字幕轨。"); return; }
  const duration = Math.min(length, timeline.duration);
  if (duration < .001) return;
  const position = clamp(start, 0, timeline.duration - duration);
  const clip = { id: crypto.randomUUID(), at: position, length: duration, name, translation: "" };
  const at = timeline.availableClipStart(track, clip, position);
  if (track.clips.some(item => at < item.at + item.length - 1e-6 && at + clip.length > item.at + 1e-6)) {
    toast("字幕轨没有足够的空间。");
    return;
  }
  clip.at = at;
  track.clips.push(clip);
  track.clips.sort((a, b) => a.at - b.at);
}

function subtitleStamp(sec) {
  return time(sec, true) + "," + String(Math.round(sec % 1 * 1000)).padStart(3, "0");
}

function exportSrt() {
  const track = locateSubtitleTrack();
  if (!track?.clips.length) { toast("当前字幕轨没有内容。"); return; }
  const srt = track.clips.slice().sort((a, b) => a.at - b.at).map((clip, i) =>
    `${i + 1}\n${subtitleStamp(clip.at)} --> ${subtitleStamp(clip.at + clip.length)}\n${clip.name}\n`).join("\n");
  const url = URL.createObjectURL(new Blob([srt], { type: "text/plain;charset=utf-8" }));
  saveLink(url, "字幕.srt");
  setTimeout(() => URL.revokeObjectURL(url), 1500);
}

function importSrt() {
  const input = document.createElement("input");
  input.type = "file";
  input.accept = ".srt";
  input.onchange = async () => {
    const file = input.files[0];
    if (!file) return;
    const parseStamp = str => str.trim().replace(",", ".").split(":").reduce((total, part) => total * 60 + Number(part), 0);
    const cues = (await file.text()).replace(/\r/g, "").trim().split(/\n\s*\n/).map(block => {
      const rows = block.split("\n");
      const index = rows.findIndex(line => line.includes("-->"));
      if (index < 0) return null;
      const [begin, end] = rows[index].split("-->").map(parseStamp);
      return { begin, end, text: rows.slice(index + 1).join(" ") };
    }).filter(cue => cue && cue.begin < cue.end && cue.end <= timeline.duration && cue.text);
    if (!cues.length) { toast("未找到该工程时长内的字幕。"); return; }
    timeline.remember();
    cues.forEach(cue => addSubtitle(cue.text, cue.begin, cue.end - cue.begin));
    timelineChange();
  };
  input.click();
}

function useScene(index) {
  state.scene = index;
  state.sceneEdit = "";
  saveDockPreferences();
  sourcesChanged();
}

function editScene(mode) {
  state.sceneEdit = state.sceneEdit === mode ? "" : mode;
  render();
  const input = $(".scene-edit input");
  input?.focus();
  input?.select();
}

function sourceDialogSelect(name, label, choices, value) {
  return `<label for="source-${name}">${tr(label)}</label>${select("source-" + name, label, choices, value,
    `id="source-${name}" name="${name}"`)}`;
}

function sourceDialogFields(item) {
  const type = sourceCatalog[item.type];
  const targetLabel = item.type === "screen" ? "显示器" : item.type === "camera" ? "设备" : type.label;
  const targets = type.targets.map(([name], index) => [index, name]);
  const field = sourceDialogSelect("target", targetLabel, targets, item.target);
  if (item.type === "image" || item.type === "region") return `<label>${tr(targetLabel)}</label><div class="hstack">
    <span data-source-detail class="truncate">${esc(item.label || "")}</span>
    <button type="button" class="btn mini" data-choose-physical>${tr(item.type === "image" ? "选择图片…" : "框选区域…")}</button></div>`;
  if (item.type === "camera") {
    return field + `<fieldset class="source-pip-fields"><legend>${tr("画中画")}</legend><div class="form">
      ${sourceDialogSelect("pipShape", "形状", ["圆角", "矩形", "圆形"], item.pipShape || "圆角")}
      <label for="source-pip-size">${tr("大小")}</label><input type="range" id="source-pip-size" name="pipSize" min="10" max="45"
        value="${clamp(Number(item.pipSize) || 23, 10, 45)}">
      <div></div><div class="source-checks hstack"><label><input type="checkbox" name="pipMirror"${item.pipMirror ? " checked" : ""}> ${tr("镜像")}</label>
      <label><input type="checkbox" name="pipBorder"${item.pipBorder !== false ? " checked" : ""}> ${tr("边框")}</label>
      <label><input type="checkbox" name="pipIsolate"${item.pipIsolate ? " checked" : ""}> ${tr("另存原素材")}</label></div></div></fieldset>`;
  }
  const cursor = `<div></div><div class="source-checks hstack"><label><input type="checkbox" name="cursor"${item.cursor !== false ? " checked" : ""}> ${tr("显示鼠标指针")}</label>
    <label><input type="checkbox" name="clicks"${item.clicks ? " checked" : ""}> ${tr("点击高亮")}</label></div>`;
  if (item.type !== "screen") return field + cursor;
  return field + cursor +
    sourceDialogSelect("screenColor", "动态范围", [["auto", "自动"], ["highlights", "保留 HDR 高光"]], item.screenColor || "auto") +
    `<label for="source-screenExposureEV">${tr("曝光补偿")}</label><input id="source-screenExposureEV" type="number" min="-2" max="2" step=".1" name="screenExposureEV" value="${Number(item.screenExposureEV) || 0}">
    <label for="source-screenWhiteNits">${tr("参考白 (nit)")}</label><input id="source-screenWhiteNits" type="number" min="0" max="1000" name="screenWhiteNits" value="${Number(item.screenWhiteNits) || 0}">`;
}

function showSourceDialog(item) {
  let type = item?.type ?? "screen";
  const create = !item;
  const body = `<form id="source-dialog-form" class="form">
    ${create ? sourceDialogSelect("type", "类型", Object.entries(sourceCatalog).map(([id, kind]) => [id, kind.label]), type) : ""}
    <div class="source-dialog-fields">${sourceDialogFields(item ?? { type, target: 0 })}</div></form>`;
  showDialog(create ? "source-add" : "source-properties", create ? "添加画面来源" : "来源设置", body,
    `<button class="btn primary" type="submit" form="source-dialog-form">${tr(create ? "添加" : "确定")}</button>`);
  const form = $("#source-dialog-form", dialog);
  const typeInput = form.elements.namedItem("type");
  let physical;
  form.addEventListener("click", async event => {
    if (!event.target.closest("[data-choose-physical]")) return;
    try {
      physical = await Native.rpc(type === "image" ? "choose-image" : "choose-region");
      $("[data-source-detail]", form).textContent = type === "image" ? physical.name : physical.slice(2).join(" × ");
    } catch (error) { reportNativeError(error); }
  });
  typeInput?.addEventListener("change", () => {
    type = typeInput.value;
    physical = undefined;
    $(".source-dialog-fields", form).innerHTML = sourceDialogFields({ type, target: 0 });
  });
  (typeInput || form.elements.namedItem("target"))?.focus();
  form.addEventListener("submit", event => {
    event.preventDefault();
    const values = new FormData(form);
    const target = Number(values.get("target"));
    if (["screen", "window", "camera"].includes(type) && !sourceCatalog[type].targets[target]) { toast("未找到设备。"); return; }
    if (type === "image" && !physical?.id && !item?.file) { toast("请选择图片。"); return; }
    if (type === "region" && !physical?.[2] && !item?.region?.[2]) { toast("请选择区域。"); return; }
    const source = item ?? makeSource(type, target);
    source.target = target;
    source.device = sourceCatalog[type].targets[target]?.[1];
    if (type === "screen") source.monitorDevice = sourceCatalog.screen.targets[target]?.[2];
    if (type === "image" && physical?.id) Object.assign(source, { mediaId: physical.id, file: physical.file, label: physical.name });
    if (type === "region" && physical?.[2]) Object.assign(source, { region: physical, label: physical.slice(2).join(" × ") });
    if (type === "camera") {
      source.pipShape = values.get("pipShape");
      source.pipSize = Number(values.get("pipSize"));
      source.pipMirror = values.has("pipMirror");
      source.pipIsolate = values.has("pipIsolate");
      source.pipBorder = values.has("pipBorder");
    } else if (type === "screen" || type === "window") {
      source.cursor = values.has("cursor");
      source.clicks = values.has("clicks");
      if (type === "screen") {
        source.screenColor = values.get("screenColor");
        source.screenWhiteNits = Number(values.get("screenWhiteNits"));
        source.screenExposureEV = Number(values.get("screenExposureEV"));
      }
    }
    if (create) {
      const scene = activeScene();
      scene.sources.push(source);
      scene.selected = source.id;
    }
    dialog.close();
    saveDockPreferences();
    sourcesChanged();
  });
}

let previewFullscreen;

function installFullscreenReviewControls(session) {
  const { panel, listeners: { signal } } = session;
  const controls = $(".player-controls", panel);
  let hideTimer;
  let heldPointer;

  function reveal() {
    panel.classList.add("playback-controls-visible");
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => {
      if (heldPointer !== undefined || controls.querySelector(":focus-visible, [aria-expanded='true']")) return;
      panel.classList.remove("playback-controls-visible");
    }, 2500);
  }

  function release(event) {
    if (event.pointerId !== heldPointer) return;
    heldPointer = undefined;
    reveal();
  }

  panel.addEventListener("pointermove", reveal, { signal });
  panel.addEventListener("pointerdown", event => {
    if (controls.contains(event.target)) heldPointer = event.pointerId;
    reveal();
  }, { signal });
  window.addEventListener("pointerup", release, { signal });
  window.addEventListener("pointercancel", release, { signal });
  panel.addEventListener("focusin", reveal, { signal });
  panel.addEventListener("focusout", reveal, { signal });
  panel.addEventListener("keydown", reveal, { signal });
  signal.addEventListener("abort", () => {
    clearTimeout(hideTimer);
    panel.classList.remove("playback-controls-visible");
  }, { once: true });
  session.revealControls = reveal;
  reveal();
}

const actions = {
  workspace: navigate,
  "app-full": () => Native.tell("window", { action: "maximize" }),
  fullscreen: kind => {
    if (previewFullscreen) return actions["leave-full"]();
    const panel = $("[data-screen='" + kind + "']");
    if (!panel) return;
    const session = { panel, listeners: new AbortController() };
    previewFullscreen = session;
    panel.classList.add("presenting");
    if (kind === "review") installFullscreenReviewControls(session);
    syncAnnotationToolbarHost();
    Native.tell("window", { action: "fullscreen" });
  },
  "leave-full": () => {
    const session = previewFullscreen;
    if (!session) return;
    previewFullscreen = null;
    session.listeners.abort();
    session.panel.classList.remove("presenting");
    syncAnnotationToolbarHost();
    Native.tell("window", { action: "restore" });
  },
  minimize: () => { state.minimized = true; render(); },
  "restore-app": () => navigate(state.page),
  "close-app": () => confirmDialog("关闭原型", "结束当前界面预览？", "confirm-close", "关闭"),
  "confirm-close": () => { persistEditorDraft(); ConversationCompact.reset(); AgentWorkspace.resetSession(); state.closed = true; dialog.close(); render(); },
  "sidebar-ai": () => {
    const pane = assistantPane();
    if (!pane) return;
    pane.open = !pane.open;
    chrome();
    drawAssistant();
    if (state.page === "editor") timeline.render();
  },
  "float-toggle": () => { state.floating = !state.floating; chrome(); },
  "float-expand": () => { state.floatSmall = !state.floatSmall; renderFloating(); },
  "floating-workbench": () => navigate("record"),
  "floating-annotate": () => {
    if (state.page !== "record" || state.minimized || state.closed) navigate("record");
    actions.annotate();
  },
  "close-dialog": () => dialog.close(),
  "open-local": () => $("#local-video").click(),
  "open-editor-project": showEditorProjectPicker,
  "import-editor-project": chooseEditorProjectFile,
  settings: value => settingsDialog(value || "general"),
  "set-theme": value => {
    const theme = ThemeManager.apply(value);
    if (dialog.open && dialogName === "settings") ThemeManager.updateSettingsSelection(dialog);
    toast("已切换主题：" + theme.name);
  },
  "compact-settings": ConversationCompact.openModelSettings,
  "clear-compact-model": ConversationCompact.clearModel,
  help: () => showDialog("help", "软件说明", '<p>可在录制工作台切换模拟画面源，在回看区播放本地示例，在剪辑工坊分割、移动和删除片段。录制、设备状态和 AI 服务使用演示数据。</p>'),
  shortcuts: () => showDialog("shortcuts", "快捷键", '<div class="form"><kbd>Alt+R</kbd><span>开始／停止录制</span><kbd>Ctrl+O</kbd><span>打开本地视频</span><kbd>Ctrl+S</kbd><span>保存剪辑工程</span><kbd>Ctrl+E</kbd><span>打开导出设置</span><kbd>空格</kbd><span>播放／暂停</span><kbd>S</kbd><span>在播放头处剪切</span><kbd>Delete</kbd><span>删除选中片段</span><kbd>Esc</kbd><span>退出全屏或关闭弹窗</span></div>'),
  export: openExport,
  "export-path": () => {
    const draft = exportDraft;
    return chooseDirectory("recorder-export", name => {
      if (!dialog.open || dialogName !== "export" || exportDraft !== draft) return;
      draft.folder = name;
      const path = $("#export-folder", dialog);
      path.value = path.title = name;
    });
  },
  "export-select-srt": () => $("#export-srt", dialog)?.click(),
  "export-clear-srt": () => { exportDraft.subtitleFile = ""; exportDialog("subtitleFile"); },
  save: saveProject,
  "reset-rate": () => { state.settings.customBitrate = false; state.settings.bitrate = recommendedBitrate(); render(); },
  "choose-output-path": chooseOutputPath,
  "preview-zoom": step => {
    const frame = $("#capture-frame");
    if (!frame) return;
    const baseline = state.zoom || frame.getBoundingClientRect().width / previewSourceSize().width * 100;
    const next = clamp(Math.round(baseline) + Math.sign(Number(step)) * 5, 1, Math.max(250, baseline));
    state.zoom = next;
    applyZoom(true);
  },
  "source-visible": id => {
    const item = activeScene().sources.find(source => source.id === id);
    item.visible = !item.visible;
    saveDockPreferences();
    sourcesChanged();
  },
  "source-select": id => {
    activeScene().selected = id;
    saveDockPreferences();
    render();
  },
  "source-picker": async (_, button) => {
    await Native.rpc("targets").then(setTargets, reportNativeError);
    if (button?.isConnected && !dialog.open) showSourceDialog();
  },
  "remove-source": () => {
    const scene = activeScene();
    const selected = activeSource();
    if (!selected) return;
    scene.sources = scene.sources.filter(item => item.id !== selected.id);
    scene.selected = scene.sources.at(-1)?.id ?? "";
    saveDockPreferences();
    sourcesChanged();
  },
  "quick-target": async (_, button) => {
    const item = activeSource();
    if (!item) return;
    if (activeChoice?.button === button) { closeChoiceMenu(); return; }
    await Native.rpc("targets").then(setTargets, reportNativeError);
    if (!button?.isConnected || item !== activeSource()) return;
    const choices = sourceCatalog[item.type].targets.map(([name], index) => [index, name]);
    toggleChoiceMenu(button, choices, item.target, value => {
      if (item.target === Number(value)) return;
      item.target = Number(value);
      item.device = sourceCatalog[item.type].targets[item.target]?.[1];
      if (item.type === "screen") item.monitorDevice = sourceCatalog.screen.targets[item.target]?.[2];
      saveDockPreferences();
      sourcesChanged();
      $("[data-action='quick-target']")?.focus({ preventScroll: true });
    });
  },
  "source-properties": () => {
    const item = activeSource();
    if (item) showSourceDialog(item);
  },
  "scene-new": () => editScene("new"),
  "scene-rename": () => editScene("rename"),
  "scene-cancel": () => { state.sceneEdit = ""; render(); },
  "choose-scene": value => useScene(Number(value)),
  "remove-scene": () => {
    if (state.scenes.length < 2) return;
    const removed = state.scenes.splice(state.scene, 1)[0];
    const next = Math.min(state.scene, state.scenes.length - 1);
    state.schedule.filter(task => task.scene === removed.name).forEach(task => { task.scene = state.scenes[next].name; });
    useScene(next);
  },
  "mixer-picker": addMixerSourceDialog,
  "mixer-add": () => {
    const id = $('input[name="mixer-source"]:checked', dialog)?.value;
    if (!availableMixerSources().includes(id)) return;
    state.audio[id] = { volume: 80, muted: false, denoise: false };
    dialog.close();
    renderMixer({ reveal: true });
    $(`[data-action="mixer-mute"][data-value="${id}"]`)?.focus({ preventScroll: true });
  },
  "mixer-remove": id => {
    if (!mixerSources[id]?.optional) return;
    delete state.audio[id];
    renderMixer();
    $("[data-action='mixer-picker']")?.focus({ preventScroll: true });
  },
  "mixer-mute": id => {
    state.audio[id].muted = !state.audio[id].muted;
    renderMixer();
    $(`[data-action="mixer-mute"][data-value="${id}"]`)?.focus({ preventScroll: true });
  },
  denoise: id => {
    if (id === "mic") state.settings.denoise = !state.settings.denoise;
    else state.audio[id].denoise = !state.audio[id].denoise;
    renderMixer();
    $(`[data-action="denoise"][data-value="${id}"]`)?.focus({ preventScroll: true });
  },
  schedule: scheduleDialog,
  "remove-schedule": index => { state.schedule.splice(Number(index), 1); scheduleDialog(); },
  countdown: (_, button) => toggleChoiceMenu(button, countdownOptions, state.settings.countdown,
    value => { state.settings.countdown = Number(value); Native.tell("preferences", { countdown: state.settings.countdown }); }),
  "record-action": recordAction,
  "pause-record": () => {
    const r = state.record;
    r.phase = r.phase === "paused" ? "recording" : "paused";
    recordTick = Date.now();
    renderRecordState();
  },
  "record-mark": () => {
    const r = state.record;
    if (!["paused", "recording"].includes(r.phase)) return;
    r.marks.push(r.seconds);
    renderRecordState();
    toast("打点已添加 · " + time(r.seconds));
  },
  screenshot: () => {
    const media = $("#capture-video") || $("#capture-image");
    if (!media) { toast("当前没有可截图的画面。"); return; }
    const canvas = document.createElement("canvas");
    canvas.width = media.videoWidth || media.naturalWidth || 1280;
    canvas.height = media.videoHeight || media.naturalHeight || 720;
    try {
      const ctx = canvas.getContext("2d");
      ctx.drawImage(media, 0, 0, canvas.width, canvas.height);
      const overlay = $(".annotation-canvas");
      if (overlay) ctx.drawImage(overlay, 0, 0, canvas.width, canvas.height);
      saveLink(canvas.toDataURL("image/png"), "预览截图.png");
    } catch {
      toast("当前预览无法生成截图。");
    }
  },
  annotate: () => {
    if (state.page !== "record" || state.minimized || state.closed) navigate("record");
    const a = state.annotation;
    a.open = !a.open;
    a.collapsed = false;
    a.point = null;
    a.flash = null;
    a.draft = null;
    if (!a.open) {
      a.effect = "";
      a.whiteboard = false;
    }
    syncAnnotationMode();
    chrome();
  },
  "annotation-tool": value => {
    state.annotation.tool = value;
    refreshAnnotationToolbar();
  },
  "annotation-menu": (key, button) => {
    const a = state.annotation;
    toggleChoiceMenu(button, annotationOptions[key].items, a[key], value => {
      a[key] = key === "clickStyle" ? value : Number(value);
      if (key === "clickStyle") a.flash = null;
      refreshAnnotationToolbar();
    });
  },
  "annotation-effect": value => {
    const a = state.annotation;
    a.effect = a.effect === value ? "" : value;
    a.point = null;
    refreshAnnotationToolbar();
  },
  "annotation-whiteboard": () => {
    state.annotation.whiteboard = !state.annotation.whiteboard;
    refreshAnnotationToolbar();
  },
  "annotation-collapse": () => {
    state.annotation.collapsed = !state.annotation.collapsed;
    refreshAnnotationToolbar();
  },
  "annotation-undo": () => moveAnnotationHistory("undo", "redo"),
  "annotation-redo": () => moveAnnotationHistory("redo", "undo"),
  "annotation-clear": () => applyAnnotationStrokes([]),
  "library-collapse": () => { state.review.collapsed = !state.review.collapsed; render(); },
  "library-filter": value => { state.review.filter = value; render(); },
  "review-select": chooseReview,
  "review-play": () => listenPlayer("review"),
  "review-seek": value => seekPlayer("review", Number(value)),
  "review-prev": () => stepChapter("review", -1),
  "review-next": () => stepChapter("review", 1),
  "player-mute": kind => playerVolume(kind, state[kind].volume ? 0 : 85, true),
  "review-mark": addReviewMark,
  "review-edit-mark": editReviewMark,
  "enter-editor": () => openEditor(Views.currentAsset(), state.review.at),
  "editor-media": openEditorProject,
  "bin-tab": value => { state.editor.bin = value; timelineChange(); },
  "still-add": saveEditorStill,
  "still-import": () => $("#editor-still-import")?.click(),
  "still-preview": id => {
    const still = editorStillItem(id);
    if (!still) return;
    if (state.editor.playing) listenPlayer("editor", false);
    if (still.at != null) seekPlayer("editor", still.at);
    state.editor.previewStill = id;
    updateEditorStillPreview();
  },
  "still-close": () => { state.editor.previewStill = ""; updateEditorStillPreview(); },
  "still-insert": openStillInsert,
  "editor-tab": value => { state.editor.tab = value; timelineChange(); },
  "editor-play": () => listenPlayer("editor"),
  "editor-seek": value => seekPlayer("editor", Number(value)),
  "timeline-seek": value => seekPlayer("editor", Number(value)),
  "editor-prev": () => stepChapter("editor", -1),
  "editor-next": () => stepChapter("editor", 1),
  "editor-mark": markDialog,
  "crop-toggle": () => {
    const editor = state.editor;
    if (!hasExplicitEditorProject()) return;
    if (editor.crop) {
      editor.cropRect = currentEditorCrop();
      editor.crop = "";
    } else {
      editor.crop = editor.cropRect ? "自由" : "16:9";
    }
    queueEditorAutosave();
    render();
  },
  "crop-ratio": value => {
    if (value === "自定义") { openCustomCrop(); return; }
    const previous = currentEditorCrop();
    state.editor.crop = value;
    state.editor.cropRect = value === "自由" ? previous : null;
    queueEditorAutosave();
    render();
  },
  undo: () => timeline.history(true),
  redo: () => timeline.history(false),
  split: () => timeline.split(),
  "clip-delete": () => timeline.remove(),
  linked: () => { timeline.linked = !timeline.linked; timeline.render(); },
  snap: () => { timeline.snap = !timeline.snap; timeline.render(); },
  "timeline-zoom": value => { timeline.zoom = clamp(timeline.zoom + Number(value), 1, 3); timeline.render(); },
  "track-select": id => {
    timeline.choose(id);
    if (id.startsWith("T")) { state.editor.subtitleTrack = id; timelineChange(); }
  },
  "track-lock": id => timeline.lock(id, "locked"),
  "track-mute": id => timeline.lock(id, "muted"),
  "track-level": id => {
    const track = timeline.tracks.find(item => item.id === id);
    if (!track || track.locked) return;
    showDialog("track-level", track.name, `<label class="hstack spaced">音量
      <input type="range" data-range="track-level" data-track="${esc(id)}" min="0" max="100" value="${Math.round((track.gain ?? 1) * 100)}">
      <output id="track-level-value">${Math.round((track.gain ?? 1) * 100)}%</output></label>`);
  },
  "add-audio": () => { timeline.addTrack("audio"); timelineChange(); },
  "add-subtitle": () => { state.editor.subtitleTrack = timeline.addTrack("subtitle"); timelineChange(); },
  "add-line": () => {
    showDialog("subtitle", "添加字幕句", `<form class="stack" data-form="subtitle">
      <textarea name="line" rows="3" required placeholder="输入字幕原文" aria-label="字幕原文"></textarea>
      <button type="submit" class="btn primary">添加到当前字幕轨</button></form>`);
  },
  "remove-line": id => {
    const track = locateSubtitleTrack();
    if (track.locked) { toast("请先解锁字幕轨。"); return; }
    timeline.remember();
    track.clips = track.clips.filter(clip => clip.id !== id);
    timelineChange();
  },
  "srt-export": exportSrt,
  "srt-import": importSrt,
  recognize: () => toast("本地识别入口已预览，素材库内带有演示字幕。"),
  translate: () => toast("翻译参数已预览。"),
  proofread: () => toast("校对入口已预览。"),
  retranslate: () => toast("单句重译入口已预览。"),
  "dub-mode": value => { state.editor.dub.mode = value; timelineChange(); },
  "dub-service": value => {
    state.editor.dub.service = value;
    state.editor.dub.voice = value === "local" ? (state.settings.modelVoice || "普通话女声") : "服务默认音色";
    timelineChange();
  },
  audition: () => toast("这里展示试听入口。"),
  "dub-generate": () => { state.editor.dub.generated = true; toast("配音生成流程已预览。"); },
  "dub-add": () => {
    const dub = state.editor.dub;
    if (!dub.generated) { toast("先体验批量生成，再加入独立音轨。"); return; }
    const id = timeline.addTrack("audio");
    const track = timeline.tracks.find(item => item.id === id);
    track.name = dub.service === "local" ? "Audio8 配音" : "在线配音";
    const subtitles = locateSubtitleTrack();
    track.clips = subtitles.clips.map(clip => ({ id: crypto.randomUUID(), at: clip.at, length: clip.length, name: track.name }));
    timelineChange();
  },
  "ai-collapse": () => { state.ai.collapsed = !state.ai.collapsed; render(); },
};

function markDialog() {
  showDialog("mark", "添加打点分节", `<form class="stack" data-form="mark">
    <small>位置 <time>${time(timeline.at)}</time></small><input type="text" name="marker" maxlength="42" required placeholder="打点名称" aria-label="打点名称">
    <button type="submit" class="btn primary">添加打点</button></form>`);
}

function onFieldChange(field, input) {
  const value = input.type === "checkbox" ? input.checked : input.value;
  if (updateNativeField(field, value)) return;
  const s = state.settings;
  const e = state.editor;
  const saveSettings = {
    encoder: "encoder", color: "color", format: "format", denoise: "denoise", language: "language",
    microphone: "microphone", "audio-mode": "audioMode", "model-voice": "modelVoice",
    "assistant-service": "assistantService", "voice-service": "voiceService",
  };
  if (Object.hasOwn(saveSettings, field)) {
    s[saveSettings[field]] = value;
    if (field === "model-voice" && e.dub.service === "local") e.dub.voice = value;
    if (!dialog.open) render();
    return;
  }
  const subtitleProps = { "subtitle-original": "name", "subtitle-translation": "translation" };
  if (Object.hasOwn(subtitleProps, field)) {
    const track = locateSubtitleTrack();
    if (track.locked) { toast("请先解锁字幕轨。"); timelineChange(); return; }
    const clip = track.clips.find(item => item.id === input.dataset.clipId);
    if (clip) {
      timeline.remember();
      clip[subtitleProps[field]] = value;
      queueEditorAutosave();
      timeline.render();
      renderSubtitleOverlay();
    }
    return;
  }
  if (field.startsWith("dub-")) {
    const [_, kind, index] = field.split("-");
    const prop = { text: "textType", script: "scripts", rule: "rules", chapter: "chapterScripts", chapterrule: "chapterRules" }[kind];
    if (prop) {
      if (kind === "chapter" && Object.values({ ...e.dub.chapterScripts, [index]: value })
        .reduce((total, text) => total + text.length, 0) > 60000) {
        input.value = e.dub.chapterScripts[index] || "";
        toast("同一工程的章节稿件最多 60000 字。");
        return;
      }
      e.dub[prop][index] = value;
      if (kind === "chapter" || kind === "chapterrule") queueEditorAutosave();
      if (kind === "text") { delete e.dub.scripts[index]; timelineChange(); }
    } else e.dub[kind] = value;
    return;
  }
  switch (field) {
    case "resolution":
    case "fps":
      s[field] = Number(value);
      if (!s.customBitrate) s.bitrate = recommendedBitrate();
      render();
      break;
    case "bitrate":
      s.bitrate = clamp(Number(value) || recommendedBitrate(), 500, 150000);
      s.customBitrate = s.bitrate !== recommendedBitrate();
      render();
      break;
    case "countdown":
      s.countdown = Number(value);
      renderRecordState();
      break;
    case "preview-fit":
      state.zoom = Number(value);
      applyZoom();
      break;
    case "ink":
      state.annotation.color = value;
      break;
    case "play-speed": {
      const kind = input.closest(".program-monitor") ? "editor" : "review";
      state[kind].speed = Number(value);
      $("#" + kind + "-video").playbackRate = state[kind].speed;
      if (kind === "editor") {
        editorTick = performance.now();
        queueEditorAutosave();
      }
      break;
    }
    case "timeline-zoom":
      timeline.zoom = Number(value);
      timeline.render();
      break;
    case "subtitle-track":
      e.subtitleTrack = value;
      e.subtitleQuery = "";
      timeline.choose(value);
      timelineChange();
      break;
    case "translation-direction": e.direction = value; break;
    case "asr-track": e.asrTrack = value; break;
    case "asr-lang": e.asrLanguage = value; break;
    case "glossary": e.glossary = value; break;
    case "voice": e.dub.voice = value; break;
  }
}

document.addEventListener("click", event => {
  const target = event.target.closest("[data-action]");
  for (const menu of $$(".shell-menu[open]")) if (!menu.contains(event.target) || target) menu.open = false;
  if (!target || target.disabled) return;
  actions[target.dataset.action]?.(target.dataset.value, target);
});

document.addEventListener("input", event => {
  const input = event.target;
  if (input.id === "library-search") {
    state.review.query = input.value;
    $(".library-list").innerHTML = Views.libraryItems();
  } else if (input.dataset.field === "subtitle-search") {
    state.editor.subtitleQuery = input.value;
    $("#subtitle-list").innerHTML = Views.subtitleRows();
  } else if (input.dataset.seek) {
    seekPlayer(input.dataset.seek, Number(input.value));
  } else {
    const value = Number(input.value);
    switch (input.dataset.range) {
      case "gain": {
        const channel = input.dataset.channel;
        state.audio[channel].volume = value;
        queueNativeMixer();
        $("[data-gain='" + channel + "']").textContent = value + "%";
        break;
      }
      case "review-volume": playerVolume("review", value); break;
      case "editor-volume": playerVolume("editor", value); break;
      case "track-level": {
        const track = timeline.tracks.find(item => item.id === input.dataset.track);
        if (track) {
          track.gain = value / 100;
          $("#track-level-value").textContent = value + "%";
          queueEditorAutosave();
        }
        break;
      }
      case "dub-speed":
        state.editor.dub.speed = value;
        $("#dub-speed").textContent = value.toFixed(2) + "×";
        break;
      case "dub-volume":
        state.editor.dub.volume = value;
        $("#dub-volume").textContent = value + "%";
        break;
    }
  }
});

document.addEventListener("change", event => {
  const input = event.target;
  if (input.id === "editor-still-import") {
    importEditorStills(input).catch(() => toast("无法导入选中的图片。"));
    return;
  }
  if (input.dataset.field) onFieldChange(input.dataset.field, input);
});

document.addEventListener("submit", event => {
  const form = event.target;
  const type = form.dataset.form;
  if (!type) return;
  event.preventDefault();
  const data = new FormData(form);
  if (type === "scene") {
    const name = String(data.get("scene")).trim();
    if (!name) return;
    const rename = state.sceneEdit === "rename";
    if (state.scenes.some((scene, i) => (!rename || i !== state.scene) && scene.name.toLowerCase() === name.toLowerCase())) {
      toast("已有同名场景。");
      return;
    }
    if (rename) {
      const oldName = activeScene().name;
      activeScene().name = name;
      state.schedule.filter(task => task.scene === oldName).forEach(task => { task.scene = name; });
    } else {
      state.scenes.push(makeScene(name));
    }
    useScene(rename ? state.scene : state.scenes.length - 1);
  } else if (type === "schedule") {
    const name = String(data.get("name")).trim();
    if (!name || !form.reportValidity()) return;
    const at = new Date(data.get("at")).getTime();
    const duration = Number(data.get("hours")) * 3600 + Number(data.get("minutes")) * 60 + Number(data.get("seconds"));
    if (!Number.isFinite(at) || !Number.isSafeInteger(duration)) return;
    addNativeSchedule(name, at, duration, data.get("scene"));
  } else if (type === "mark") {
    const name = String(data.get("marker")).trim();
    if (!name) return;
    dialog.close();
    timeline.addMarker(name);
  } else if (type === "editor-crop") {
    state.editor.cropRatio = { width: Number(data.get("width")), height: Number(data.get("height")) };
    state.editor.crop = "自定义";
    state.editor.cropRect = null;
    dialog.close();
    queueEditorAutosave();
    render();
  } else if (type === "editor-still") {
    const still = editorStillItem(form.dataset.still);
    const length = Number(data.get("length"));
    if (!still || length < .1 || length > 600 || !Number.isFinite(length)) return;
    if (timeline.insertStill(still, length, String(data.get("caption")).trim())) {
      dialog.close();
      queueEditorAutosave();
    }
  } else if (type === "subtitle") {
    const name = String(data.get("line")).trim();
    if (!name) return;
    timeline.remember();
    addSubtitle(name);
    dialog.close();
    timelineChange();
  }
});

$("#local-video").addEventListener("change", event => {
  const input = event.target;
  if (!input.files.length) return;
  const file = input.files[0];
  input.value = "";
  const src = URL.createObjectURL(file);
  const media = document.createElement("video");
  media.preload = "metadata";
  media.onloadedmetadata = () => {
    if (!Number.isFinite(media.duration)) { toast("无法读取这段视频的时长。"); URL.revokeObjectURL(src); return; }
    const asset = { id: crypto.randomUUID(), title: file.name.replace(/\.[^.]+$/, ""), name: file.name,
      date: "本地视频", kind: "normal", src, duration: media.duration, marks: [], thumbnails: [], local: true };
    assets.push(asset);
    registerEditorVideoFile(file, asset);
    state.review.query = "";
    state.review.filter = "all";
    chooseReview(asset.id);
    collectThumbnails(media, asset);
  };
  media.onerror = () => { URL.revokeObjectURL(src); toast("当前浏览器无法打开这个视频格式。"); };
  media.src = src;
});

async function collectThumbnails(media, asset, signal) {
  if (!media.videoWidth || !media.videoHeight) return;
  const canvas = document.createElement("canvas");
  canvas.width = 240;
  canvas.height = Math.round(canvas.width * media.videoHeight / media.videoWidth);
  const ctx = canvas.getContext("2d");
  const count = clamp(Math.ceil(asset.duration / 2), 8, 24);
  for (let i = 0; i < count && !signal?.aborted; i++) {
    const at = asset.duration * (i + 0.5) / count;
    const ready = await new Promise(resolve => {
      const finish = event => {
        clearTimeout(wait);
        media.removeEventListener("seeked", finish);
        signal?.removeEventListener("abort", finish);
        resolve(event?.type === "seeked" && media.readyState >= 2);
      };
      const wait = setTimeout(finish, 1200);
      media.addEventListener("seeked", finish, { once: true });
      signal?.addEventListener("abort", finish, { once: true });
      media.currentTime = at;
    });
    if (!ready) break;
    ctx.drawImage(media, 0, 0, canvas.width, canvas.height);
    asset.thumbnails.push({ at, src: canvas.toDataURL("image/jpeg", .75) });
    if (!asset.poster) {
      asset.poster = asset.thumbnails[0].src;
      if (state.page === "review") $(".library-list").innerHTML = Views.libraryItems();
    }
    if (timeline.media === asset.id && state.page === "editor" && (i === 0 || i === count - 1)) timeline.render();
  }
  media.removeAttribute("src");
  media.load();
}

function adjustDivider(name, clientX, clientY) {
  if (name === "assistant") {
    const pane = assistantPane();
    if (!pane?.open) return;
    const shell = $("#app-main");
    const { width, right } = shell.getBoundingClientRect();
    pane.width = clamp(Math.round(right - clientX), Math.min(200, width * .38), Math.min(540, width * .43));
    shell.style.setProperty("--assistant-width", pane.width + "px");
    if (state.page === "editor") timeline.render();
    return;
  }
  const page = $(".page");
  const bounds = page.getBoundingClientRect();
  if (name.startsWith("editor-")) {
    const layout = state.editorLayout;
    const upper = $(".editor-upper", page).getBoundingClientRect();
    const previewMin = Math.min(315, upper.width * .37);
    if (name === "editor-left") {
      const remaining = upper.width - $(".editor-options", page).offsetWidth - previewMin - 10;
      layout.left = Math.round(clamp(clientX - upper.left, Math.min(120, upper.width * .24), Math.min(upper.width * .34, remaining)));
      page.style.setProperty("--editor-left", layout.left + "px");
    } else if (name === "editor-right") {
      const remaining = upper.width - $(".editor-bin", page).offsetWidth - previewMin - 10;
      layout.right = Math.round(clamp(upper.right - clientX, Math.min(160, upper.width * .32), Math.min(upper.width * .46, remaining)));
      page.style.setProperty("--editor-right", layout.right + "px");
    } else {
      layout.timeline = clamp(100 * (bounds.bottom - clientY) / bounds.height, 16, 66);
      page.style.setProperty("--editor-timeline", layout.timeline + "%");
    }
  } else if (name.startsWith("dock-")) {
    const grid = $(".register-docks", page);
    const panels = $$("[data-dock]", grid);
    const widths = panels.map(panel => panel.getBoundingClientRect().width);
    const index = Number(name.slice(5));
    const total = widths[index] + widths[index + 1];
    const minimums = { scene: 102, source: 125, mixer: 168, output: 208, control: 168 };
    const leftMin = Math.min(minimums[state.dockOrder[index]], total * .48);
    const rightMin = Math.min(minimums[state.dockOrder[index + 1]], total * .48);
    widths[index] = clamp(clientX - panels[index].getBoundingClientRect().left - 1, leftMin, total - rightMin);
    widths[index + 1] = total - widths[index];
    const sum = widths.reduce((a, width) => a + width, 0);
    state.width.docks = widths.map(width => width / sum * 100);
    grid.style.setProperty("--dock-widths", dockColumns());
  } else if (name === "docks") {
    state.height.docks = clamp(Math.round(bounds.bottom - clientY), 174, Math.max(174, Math.min(275, bounds.height - 255)));
    page.style.setProperty("--dock-height", state.height.docks + "px");
  } else {
    const right = bounds.right - clientX;
    const left = $(".ai-nav")?.getBoundingClientRect().right ?? bounds.left;
    state.ai.hide = right < 69 ? "right" : clientX - left < 73 ? "mid" : "";
    state.width.ai = clamp(100 * right / bounds.width, 19, 61);
    page.style.setProperty("--ai-right", state.width.ai + "%");
    page.classList.toggle("ai-no-mid", state.ai.hide === "mid");
    page.classList.toggle("ai-no-right", state.ai.hide === "right");
  }
}

function swapRecordingDocks(first, second) {
  if (!second || first === second) return;
  const grid = first.parentElement;
  const order = state.dockOrder;
  const widths = state.width.docks;
  const a = order.indexOf(first.dataset.dock);
  const b = order.indexOf(second.dataset.dock);
  const previousFirstNext = first.nextElementSibling;
  grid.insertBefore(first, second.nextElementSibling);
  grid.insertBefore(second, previousFirstNext);
  [order[a], order[b]] = [order[b], order[a]];
  [widths[a], widths[b]] = [widths[b], widths[a]];
  grid.style.setProperty("--dock-widths", dockColumns());
  $$(".splitter", grid).forEach((divider, index) => divider.setAttribute("aria-label", dockDividerLabel(index)));
  $("[data-dock-handle]", first).focus({ preventScroll: true });
  saveDockPreferences();
}

document.addEventListener("pointerdown", event => {
  const handle = event.target.closest("[data-dock-handle]");
  if (!handle || !event.isPrimary || event.button !== 0 || event.target.closest("button, select, input, a")) return;
  const first = handle.closest("[data-dock]");
  const panels = $$("[data-dock]", first.parentElement);
  const { clientX: x, clientY: y, pointerId } = event;
  let started = false;
  let target;
  handle.setPointerCapture(pointerId);
  function move(ev) {
    if (!started && Math.hypot(ev.clientX - x, ev.clientY - y) < 5) return;
    if (!started) {
      started = true;
      first.classList.add("dock-dragging");
    }
    const found = panels.find(panel => {
      const rect = panel.getBoundingClientRect();
      return panel !== first && ev.clientX >= rect.left && ev.clientX <= rect.right
        && ev.clientY >= rect.top && ev.clientY <= rect.bottom;
    });
    if (target !== found) {
      target?.classList.remove("dock-drop-target");
      target = found;
      target?.classList.add("dock-drop-target");
    }
  }
  function cleanup() {
    first.classList.remove("dock-dragging");
    target?.classList.remove("dock-drop-target");
    handle.removeEventListener("pointermove", move);
    handle.removeEventListener("pointerup", drop);
    handle.removeEventListener("lostpointercapture", cleanup);
  }
  function drop(ev) {
    move(ev);
    cleanup();
    if (started) swapRecordingDocks(first, target);
  }
  handle.addEventListener("pointermove", move);
  handle.addEventListener("pointerup", drop);
  handle.addEventListener("lostpointercapture", cleanup);
});

document.addEventListener("pointerdown", event => {
  const divider = event.target.closest("[data-divider]");
  if (!divider || event.button !== 0) return;
  event.preventDefault();
  divider.setPointerCapture(event.pointerId);
  divider.classList.add("dragging");
  const move = ev => adjustDivider(divider.dataset.divider, ev.clientX, ev.clientY);
  divider.addEventListener("pointermove", move);
  divider.addEventListener("lostpointercapture", () => {
    divider.removeEventListener("pointermove", move);
    divider.classList.remove("dragging");
    if (divider.dataset.divider.startsWith("dock-")) saveDockPreferences();
  }, { once: true });
});

document.addEventListener("keydown", event => {
  if (event.altKey && event.target.matches("[data-dock-handle]") && ["ArrowLeft", "ArrowRight"].includes(event.key)) {
    const dock = event.target.closest("[data-dock]");
    const panels = $$("[data-dock]", dock.parentElement);
    const other = panels[panels.indexOf(dock) + (event.key === "ArrowLeft" ? -1 : 1)];
    event.preventDefault();
    swapRecordingDocks(dock, other);
    return;
  }
  if (event.key === "Escape" && event.target.closest(".scene-edit")) {
    actions["scene-cancel"]();
    $(".scene-choice[aria-pressed='true']")?.focus();
    event.preventDefault();
    return;
  }
  if (event.key === "Escape" && previewFullscreen) {
    actions["leave-full"]();
    event.preventDefault();
    return;
  }
  const divider = event.target.closest("[data-divider]");
  if (divider && /^Arrow(Left|Right|Up|Down)$/.test(event.key)) {
    const rect = divider.getBoundingClientRect();
    const x = (event.key === "ArrowRight" ? 16 : event.key === "ArrowLeft" ? -16 : 0) + rect.left;
    const y = (event.key === "ArrowDown" ? 12 : event.key === "ArrowUp" ? -12 : 0) + rect.top;
    adjustDivider(divider.dataset.divider, x, y);
    if (divider.dataset.divider.startsWith("dock-")) saveDockPreferences();
    event.preventDefault();
    return;
  }
  const typing = event.target.matches("input, textarea, select, [contenteditable]");
  const frameNudge = state.page === "editor" && !dialog.open && !event.altKey && !event.ctrlKey &&
    !event.metaKey && ["ArrowLeft", "ArrowRight"].includes(event.key) &&
    (!typing || event.target.matches("[data-seek='editor']")) &&
    (event.target.closest("#workspace") || event.target === document.body);
  if (frameNudge) {
    if (state.editor.playing) listenPlayer("editor", false);
    timeline.stepFrame(event.key === "ArrowRight" ? 1 : -1);
    event.preventDefault();
    return;
  }
  if (typing || dialog.open) return;
  if (event.altKey && event.key.toLowerCase() === "r") { recordAction(); event.preventDefault(); return; }
  if (event.ctrlKey && ["o", "s", "e"].includes(event.key.toLowerCase())) {
    ({ o: actions["open-local"], s: actions.save, e: actions.export })[event.key.toLowerCase()]();
    event.preventDefault();
    return;
  }
  if (event.key === " " && !event.target.matches("button") && ["review", "editor"].includes(state.page)) {
    listenPlayer(state.page);
    event.preventDefault();
  }
  if (state.page !== "editor") return;
  if (event.key.toLowerCase() === "s" && !event.ctrlKey && !event.altKey) timeline.split();
  if (event.key === "Delete") timeline.remove();
  if (event.ctrlKey && /^(z|y)$/i.test(event.key)) { timeline.history(event.key.toLowerCase() === "z" && !event.shiftKey); event.preventDefault(); }
  if (event.key === "Enter" && event.target.matches("[data-clip]")) timeline.choose(event.target.dataset.track, event.target.dataset.clip);
});

$("#titlebar").addEventListener("keydown", event => {
  const menu = event.target.closest(".shell-menu");
  if (!menu) return;
  if (event.key === "Escape" && menu.open) {
    menu.open = false;
    $("summary", menu).focus();
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
  if (!menu.open && (event.key === "Home" || event.key === "End")) return;
  for (const other of $$(".shell-menu[open]")) if (other !== menu) other.open = false;
  menu.open = true;
  const items = $$(".popup button:not(:disabled)", menu);
  const at = items.indexOf(event.target);
  const next = {
    ArrowDown: (at + 1) % items.length,
    ArrowUp: (at < 0 ? items.length - 1 : (at - 1 + items.length) % items.length),
    Home: 0,
    End: items.length - 1,
  }[event.key];
  items[next]?.focus();
  event.preventDefault();
});

$("#titlebar").addEventListener("dblclick", event => {
  if (!event.target.closest("button, summary")) actions["app-full"]();
});
window.addEventListener("resize", () => {
  placeFloatingBar();
  closeChoiceMenu();
  if (state.page === "editor") timeline.render();
  drawAnnotations();
});

function annotationPoint(event, canvas) {
  const rect = canvas.getBoundingClientRect();
  return [clamp((event.clientX - rect.left) / rect.width, 0, 1), clamp((event.clientY - rect.top) / rect.height, 0, 1)];
}

function configureAnnotationCanvas(canvas) {
  if (!canvas) return;
  const a = state.annotation;
  canvas.dataset.tool = a.tool;
  canvas.style.pointerEvents = a.tool === "cursor" && !a.effect && a.clickStyle === "none" ? "none" : "";
}

const annotationToolbarMargin = 6;
const annotationToolbarObserver = new ResizeObserver(() => placeAnnotationToolbar($(".annotation-toolbar")));

function syncAnnotationMode() {
  const a = state.annotation;
  const frame = $("#capture-frame");
  if (!frame) return;
  $(".record-monitor [data-action='annotate']")?.setAttribute("aria-pressed", String(a.open));
  if (a.open) {
    frame.insertAdjacentHTML("beforeend", '<canvas class="annotation-canvas" aria-label="画面标注区域"></canvas>');
    document.body.insertAdjacentHTML("beforeend", Views.annotationTools());
    setupAnnotations();
  } else {
    closeChoiceMenu();
    cancelAnimationFrame(frameHandle);
    annotationToolbarObserver.disconnect();
    $(".annotation-text", frame)?.remove();
    $(".annotation-canvas", frame)?.remove();
    $(".annotation-toolbar")?.remove();
  }
}

function syncAnnotationToolbarHost(toolbar = $(".annotation-toolbar")) {
  if (!toolbar) return;
  const fullMonitor = document.fullscreenElement?.matches(".record-monitor") && document.fullscreenElement;
  const host = fullMonitor || $(".record-monitor.presenting") || document.body;
  if (toolbar.parentElement !== host) host.append(toolbar);
  placeAnnotationToolbar(toolbar);
}

function annotationToolbarTravel(toolbar) {
  return [
    Math.max(0, window.innerWidth - toolbar.offsetWidth - 2 * annotationToolbarMargin),
    Math.max(0, window.innerHeight - toolbar.offsetHeight - 2 * annotationToolbarMargin),
  ];
}

function placeAnnotationToolbar(toolbar) {
  const anchor = $(".record-canvas-wrap");
  if (!toolbar || !anchor) return;
  const [width, height] = annotationToolbarTravel(toolbar);
  const margin = annotationToolbarMargin;
  const position = state.annotation.toolbarPosition;
  const preview = anchor.getBoundingClientRect();
  const x = position ? width * position[0] : (preview.left + preview.right - toolbar.offsetWidth) / 2 - margin;
  const y = position ? height * position[1] : preview.bottom - toolbar.offsetHeight - 2 * margin;
  toolbar.style.left = Math.round(margin + clamp(x, 0, width)) + "px";
  toolbar.style.top = Math.round(margin + clamp(y, 0, height)) + "px";
}

function installAnnotationToolbar(toolbar) {
  annotationToolbarObserver.disconnect();
  syncAnnotationToolbarHost(toolbar);
  annotationToolbarObserver.observe(document.documentElement);
  annotationToolbarObserver.observe($(".record-canvas-wrap"));
  annotationToolbarObserver.observe(toolbar);
  const grip = $(".annotation-grip", toolbar);
  let drag;
  function move(left, top) {
    const travel = annotationToolbarTravel(toolbar);
    state.annotation.toolbarPosition = [left, top].map((coordinate, axis) => travel[axis] > 0
      ? clamp((coordinate - annotationToolbarMargin) / travel[axis], 0, 1)
      : (state.annotation.toolbarPosition?.[axis] ?? 0));
    placeAnnotationToolbar(toolbar);
  }
  grip.addEventListener("pointerdown", event => {
    if (event.button !== 0 || !event.isPrimary) return;
    const rect = toolbar.getBoundingClientRect();
    drag = { x: event.clientX - rect.left, y: event.clientY - rect.top };
    grip.setPointerCapture(event.pointerId);
  });
  grip.addEventListener("pointermove", event => {
    if (!drag || !grip.hasPointerCapture(event.pointerId)) return;
    move(event.clientX - drag.x, event.clientY - drag.y);
  });
  grip.addEventListener("lostpointercapture", () => { drag = null; });
  grip.addEventListener("keydown", event => {
    const offset = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[event.key];
    if (!offset) return;
    event.preventDefault();
    const step = event.shiftKey ? 24 : 8;
    const rect = toolbar.getBoundingClientRect();
    move(rect.left + offset[0] * step, rect.top + offset[1] * step);
  });
}

function refreshAnnotationToolbar() {
  const toolbar = $(".annotation-toolbar");
  if (toolbar) {
    const focus = toolbar.contains(document.activeElement) ? document.activeElement.dataset : null;
    const template = document.createElement("template");
    template.innerHTML = Views.annotationTools();
    const next = template.content.firstElementChild;
    if (next) {
      toolbar.replaceWith(next);
      installAnnotationToolbar(next);
      if (focus?.action) {
        const trigger = [...next.querySelectorAll("[data-action]")].find(btn =>
          !btn.disabled && btn.dataset.action === focus.action && btn.dataset.value === focus.value);
        trigger?.focus({ preventScroll: true });
      }
    }
  }
  configureAnnotationCanvas($(".annotation-canvas"));
  drawAnnotations();
}

function applyAnnotationStrokes(strokes, previous = state.annotation.strokes) {
  const a = state.annotation;
  a.undo.push(previous);
  if (a.undo.length > 60) a.undo.shift();
  a.redo = [];
  a.strokes = strokes;
  refreshAnnotationToolbar();
}

function moveAnnotationHistory(from, to) {
  const a = state.annotation;
  if (!a[from].length) return;
  a[to].push(a.strokes);
  a.strokes = a[from].pop();
  refreshAnnotationToolbar();
}

function annotationStrokeAt(stroke, point, w, h) {
  const points = stroke.points.map(([x, y]) => [x * w, y * h]);
  const [px, py] = [point[0] * w, point[1] * h];
  const [first, last] = [points[0], points.at(-1)];
  const threshold = stroke.size * (["blur", "highlight"].includes(stroke.part) ? 2 : 1) + 6;
  const closeToEdge = ([ax, ay], [bx, by]) => {
    const [dx, dy] = [bx - ax, by - ay];
    const fraction = clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy || 1), 0, 1);
    return Math.hypot(px - ax - fraction * dx, py - ay - fraction * dy) <= threshold;
  };
  if (stroke.part === "text") {
    const textSize = Math.max(16, stroke.size * 4);
    return px >= first[0] - threshold && px <= first[0] + textSize * stroke.text.length
      && py >= first[1] - threshold && py <= first[1] + textSize + threshold;
  }
  if (["rect", "mask"].includes(stroke.part)) {
    const [left, right] = [Math.min(first[0], last[0]), Math.max(first[0], last[0])];
    const [top, bottom] = [Math.min(first[1], last[1]), Math.max(first[1], last[1])];
    return px >= left - threshold && px <= right + threshold && py >= top - threshold && py <= bottom + threshold
      && (stroke.part === "mask" || Math.min(Math.abs(px - left), Math.abs(px - right), Math.abs(py - top), Math.abs(py - bottom)) <= threshold);
  }
  return points.some((end, i) => closeToEdge(points[i - 1] ?? end, end));
}

function setupAnnotations() {
  const canvas = $(".annotation-canvas");
  if (!canvas) return;
  installAnnotationToolbar($(".annotation-toolbar"));
  const a = state.annotation;
  let eraserBefore;
  function erase(point) {
    const index = a.strokes.findLastIndex(stroke => annotationStrokeAt(stroke, point, canvas.width, canvas.height));
    if (index >= 0) a.strokes = a.strokes.toSpliced(index, 1);
  }
  configureAnnotationCanvas(canvas);
  canvas.onpointerdown = event => {
    if (event.button !== 0 || !event.isPrimary) return;
    const point = annotationPoint(event, canvas);
    a.point = point;
    if (a.clickStyle !== "none") a.flash = { point, at: performance.now() };
    if (a.tool === "cursor") {
      drawAnnotations();
      return;
    }
    event.preventDefault();
    if (a.tool === "text") {
      const old = $(".annotation-text");
      if (old) old.blur();
      const input = document.createElement("input");
      input.type = "text";
      input.className = "annotation-text";
      input.style.left = pct(Math.min(point[0], .62), 1);
      input.style.top = pct(Math.min(point[1], .9), 1);
      input.placeholder = "输入标注文字";
      input.setAttribute("aria-label", "输入标注文字");
      canvas.parentNode.append(input);
      input.focus();
      input.onblur = () => {
        const text = input.value.trim();
        input.remove();
        if (text) applyAnnotationStrokes([...a.strokes, { part: "text", text, points: [point], color: a.color, size: a.size }]);
      };
      input.onkeydown = key => {
        if (key.key === "Escape") input.value = "";
        if (key.key === "Enter" || key.key === "Escape") input.blur();
      };
      return;
    }
    canvas.setPointerCapture(event.pointerId);
    if (a.tool === "eraser") {
      eraserBefore = a.strokes;
      erase(point);
    } else {
      a.draft = { part: a.tool, color: a.color, size: a.size, blur: a.blur, points: [point] };
    }
    drawAnnotations();
  };
  canvas.onpointermove = event => {
    if (!event.isPrimary) return;
    const point = annotationPoint(event, canvas);
    a.point = point;
    if (canvas.hasPointerCapture(event.pointerId)) {
      if (eraserBefore) erase(point);
      if (a.draft) {
        const points = a.draft.points;
        if (["rect", "mask", "arrow"].includes(a.draft.part)) points[1] = point;
        else points.push(point);
      }
    }
    if (eraserBefore || a.draft || a.effect) drawAnnotations();
  };
  canvas.onpointerup = () => {
    if (eraserBefore) {
      const previous = eraserBefore;
      eraserBefore = null;
      if (previous !== a.strokes) applyAnnotationStrokes(a.strokes, previous);
    }
    if (a.draft) {
      const committed = a.draft;
      a.draft = null;
      applyAnnotationStrokes([...a.strokes, committed]);
    }
  };
  canvas.onpointercancel = () => {
    if (eraserBefore) a.strokes = eraserBefore;
    eraserBefore = null;
    a.draft = null;
    a.point = null;
    drawAnnotations();
  };
  canvas.onpointerleave = () => {
    a.point = null;
    if (a.effect) drawAnnotations();
  };
  let lastPaint = 0;
  const animate = now => {
    const liveMedia = $("#capture-video");
    const movingPixels = !a.whiteboard && liveMedia && !liveMedia.paused && (a.strokes.some(stroke => stroke.part === "blur") || a.effect === "magnifier");
    if ((a.flash || movingPixels) && now - lastPaint > 30) {
      lastPaint = now;
      drawAnnotations();
    }
    frameHandle = requestAnimationFrame(animate);
  };
  drawAnnotations();
  frameHandle = requestAnimationFrame(animate);
}

const annotationScratch = document.createElement("canvas");
const annotationZoom = document.createElement("canvas");

function annotationBuffer(canvas, w, h) {
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, w, h);
  return ctx;
}

function paintAnnotationMedia(ctx, w, h) {
  const frame = $("#capture-frame");
  if (!frame) return false;
  const video = $("#capture-video", frame);
  const image = $("#capture-image", frame);
  const media = video?.readyState >= 2 ? video : image?.complete && image.naturalWidth ? image : null;
  if (!media) return false;
  const sw = media.videoWidth || media.naturalWidth;
  const sh = media.videoHeight || media.naturalHeight;
  const fit = Math.min(w / sw, h / sh);
  ctx.fillStyle = "#07090a";
  ctx.fillRect(0, 0, w, h);
  ctx.drawImage(media, (w - fit * sw) / 2, (h - fit * sh) / 2, fit * sw, fit * sh);

  const camera = $(".pip", frame);
  if (camera?.complete && camera.naturalWidth) {
    const viewport = frame.getBoundingClientRect();
    const box = camera.getBoundingClientRect();
    const [x, y] = [box.x - viewport.x, box.y - viewport.y];
    const roomWidth = box.width;
    const roomHeight = box.height;
    const cover = Math.max(roomWidth / camera.naturalWidth, roomHeight / camera.naturalHeight);
    const [dw, dh] = [cover * camera.naturalWidth, cover * camera.naturalHeight];
    ctx.save();
    ctx.beginPath();
    if (camera.classList.contains("round")) {
      ctx.ellipse(x + roomWidth / 2, y + roomHeight / 2, roomWidth / 2, roomHeight / 2, 0, 0, Math.PI * 2);
    } else {
      ctx.roundRect(x, y, roomWidth, roomHeight, 4);
    }
    ctx.clip();
    ctx.translate(x + roomWidth / 2, y + roomHeight / 2);
    if (pip.dataset.mirrored === "true") ctx.scale(-1, 1);
    ctx.drawImage(camera, -dw / 2, -dh / 2, dw, dh);
    ctx.restore();
  }
  return true;
}

function drawAnnotations() {
  const canvas = $(".annotation-canvas");
  if (!canvas || !canvas.clientWidth || !canvas.clientHeight) return;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (w !== canvas.width || h !== canvas.height) { canvas.width = w; canvas.height = h; }
  const ctx = canvas.getContext("2d");
  const a = state.annotation;
  ctx.clearRect(0, 0, w, h);
  if (a.whiteboard) {
    ctx.fillStyle = "#f3f5f7";
    ctx.fillRect(0, 0, w, h);
  }
  const list = [...a.strokes, ...(a.draft ? [a.draft] : [])];
  for (const stroke of [...list.filter(item => item.part === "blur"), ...list.filter(item => item.part !== "blur" && item.part !== "mask"), ...list.filter(item => item.part === "mask")]) {
    const points = stroke.points.map(([x, y]) => [x * w, y * h]);
    const [x, y] = points[0];
    const [ex, ey] = points.at(-1);
    ctx.save();
    ctx.fillStyle = stroke.color;
    ctx.strokeStyle = stroke.color;
    ctx.lineWidth = stroke.size;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    if (stroke.part === "blur") {
      if (!a.whiteboard) {
        const mask = annotationBuffer(annotationScratch, w, h);
        mask.save();
        mask.filter = `blur(${stroke.blur}px)`;
        if (paintAnnotationMedia(mask, w, h)) {
          mask.filter = "none";
          mask.globalCompositeOperation = "destination-in";
          mask.lineCap = mask.lineJoin = "round";
          mask.lineWidth = 4 * stroke.size;
          mask.beginPath();
          mask.moveTo(x, y);
          for (const point of points.slice(1)) mask.lineTo(...point);
          if (points.length === 1) mask.lineTo(x + .01, y);
          mask.stroke();
          ctx.drawImage(annotationScratch, 0, 0);
        }
        mask.restore();
      }
    } else if (stroke.part === "rect" || stroke.part === "mask") {
      if (stroke.part === "mask") ctx.fillRect(x, y, ex - x, ey - y);
      else ctx.strokeRect(x, y, ex - x, ey - y);
    } else if (stroke.part === "text") {
      ctx.font = `${Math.max(16, stroke.size * 4)}px Microsoft YaHei UI`;
      ctx.textBaseline = "top";
      ctx.fillText(stroke.text, x, y);
    } else {
      if (stroke.part === "highlight") { ctx.globalAlpha = .38; ctx.lineWidth *= 3; }
      ctx.beginPath();
      ctx.moveTo(x, y);
      for (const point of points.slice(1)) ctx.lineTo(...point);
      if (points.length === 1) ctx.lineTo(x + .01, y);
      if (stroke.part === "arrow" && points.length > 1) {
        const angle = Math.atan2(ey - y, ex - x);
        for (const wing of [-.52, .52]) {
          ctx.moveTo(ex, ey);
          ctx.lineTo(ex - 15 * Math.cos(angle + wing), ey - 15 * Math.sin(angle + wing));
        }
      }
      ctx.stroke();
    }
    ctx.restore();
  }
  drawAnnotationEffects(ctx, a, w, h);
}

function drawAnnotationEffects(ctx, a, w, h) {
  if (a.point && a.effect) {
    const [x, y] = [a.point[0] * w, a.point[1] * h];
    const radius = clamp(Math.min(w, h) * .18, 12, 115);
    ctx.save();
    if (a.effect === "spotlight") {
      ctx.fillStyle = "#070b11a6";
      ctx.beginPath();
      ctx.rect(0, 0, w, h);
      ctx.arc(x, y, radius, 0, Math.PI * 2);
      ctx.fill("evenodd");
    } else {
      const zoom = annotationBuffer(annotationZoom, w, h);
      paintAnnotationMedia(zoom, w, h);
      zoom.drawImage(ctx.canvas, 0, 0);
      const centerX = clamp(x, radius, w - radius);
      const centerY = clamp(y, radius, h - radius);
      ctx.beginPath();
      ctx.arc(centerX, centerY, radius, 0, Math.PI * 2);
      ctx.clip();
      ctx.fillStyle = "#111820";
      ctx.fillRect(0, 0, w, h);
      ctx.drawImage(annotationZoom, centerX - x * 2, centerY - y * 2, w * 2, h * 2);
      ctx.strokeStyle = "#d1e4fb";
      ctx.lineWidth = 3;
      ctx.stroke();
    }
    ctx.restore();
  }
  if (!a.flash) return;
  const progress = (performance.now() - a.flash.at) / 480;
  if (progress >= 1) {
    a.flash = null;
    return;
  }
  const [fx, fy] = [a.flash.point[0] * w, a.flash.point[1] * h];
  ctx.save();
  ctx.globalAlpha = 1 - progress;
  ctx.strokeStyle = "#8ec8ff";
  ctx.fillStyle = "#ffdf83aa";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(fx, fy, a.clickStyle === "ripple" ? 8 + progress * 23 : 13, 0, Math.PI * 2);
  if (a.clickStyle === "highlight") ctx.fill();
  else ctx.stroke();
  ctx.restore();
}


