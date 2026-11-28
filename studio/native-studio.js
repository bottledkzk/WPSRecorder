"use strict";

const nativeSettings = new Map([
  ["format", "format"], ["encoder", "encoder"], ["color", "color"], ["audio-mode", "audioMode"],
  ["countdown", "countdown"], ["resolution", "resolution"], ["fps", "fps"], ["bitrate", "bitrate"],
]);
let mixerTimer;
let displayedMessage;
let requestingClose = false;
const previewing = new Set();
const monitorTracks = new Map();
let editorMonitorFrame = 0;
let latestReviewSelection = 0;
let reviewProbe;
let thumbnailJob;
let captionDrag;
let speechPreview;
let speechWorking = false;

function hasEditorMonitorAudio() {
  return monitorTracks.size > 0;
}

function syncEditorMonitor() {
  if (state.page !== "editor" || !hasExplicitEditorProject()) return;
  const tracks = timeline.tracks.filter(item => item.kind === "audio");
  const chapter = tracks.find(track => track.chapterDubbing && !track.muted && track.gain > 0 && track.originalGain < 1);
  const narration = chapter?.clips.find(clip => clip.at <= timeline.at && timeline.at < clip.at + clip.length);
  const spokenAt = narration ? narration.source + timeline.at - narration.at : -1;
  const ducking = narration && chapter.speech?.some(range => range.at <= spokenAt && spokenAt < range.at + range.length);
  const current = new Set(tracks.map(track => track.id));
  for (const [id, item] of monitorTracks) {
    if (current.has(id)) continue;
    item.player?.pause();
    monitorTracks.delete(id);
  }
  for (const track of tracks) {
    let item = monitorTracks.get(track.id);
    if (!item && track.stream >= 0) {
      item = {};
      monitorTracks.set(track.id, item);
      Native.rpc("audio-preview", { id: Views.editorAsset().id, stream: track.stream }).then(url => {
        if (monitorTracks.get(track.id) !== item) return;
        if (url) item.player = new Audio(url);
      }).catch(() => {});
    } else if (!item && track.src) {
      item = { player: new Audio(track.src) };
      monitorTracks.set(track.id, item);
    }
    const player = item?.player;
    if (!player) continue;
    const clip = track.clips.find(clip => clip.at <= timeline.at && timeline.at < clip.at + clip.length);
    const play = state.editor.playing && clip && !track.muted && state.editor.volume > 0 && !state.editor.previewStill;
    if (!play) { if (!player.paused) player.pause(); continue; }
    const desired = (clip.source || 0) + timeline.at - clip.at;
    if (player.readyState && (item.clip !== clip.id || Math.abs(player.currentTime - desired) > .22))
      player.currentTime = desired;
    item.clip = clip.id;
    const gain = ducking && track !== chapter ? chapter.originalGain : 1;
    const targetGain = clamp(gain * (track.gain ?? 1) * state.editor.volume / 100, 0, 1);
    player.volume = Math.abs(player.volume - targetGain) < .01 ? targetGain : (targetGain + player.volume) / 2;
    player.playbackRate = state.editor.speed;
    if (player.paused) player.play().catch(() => {});
  }
  const video = $("#editor-video");
  if (video && hasEditorMonitorAudio()) video.muted = true;
}

function scheduleEditorMonitor() {
  if (editorMonitorFrame || !state.editor.playing) return;
  let previous = 0;
  const tick = stamp => {
    editorMonitorFrame = 0;
    if (state.page !== "editor" || !state.editor.playing || state.minimized || state.closed) {
      stopEditorMonitor();
      return;
    }
    if (stamp - previous >= 33) {
      previous = stamp;
      advanceEditor();
      syncEditorMonitor();
    }
    if (state.editor.playing) editorMonitorFrame = requestAnimationFrame(tick);
  };
  editorMonitorFrame = requestAnimationFrame(tick);
}

function stopEditorMonitor(release = false) {
  cancelAnimationFrame(editorMonitorFrame);
  editorMonitorFrame = 0;
  for (const { player } of monitorTracks.values()) {
    player?.pause();
    if (release && player) {
      player.removeAttribute("src");
      player.load();
    }
  }
  if (release) monitorTracks.clear();
}

document.addEventListener("editorprojectchange", () => {
  stopEditorMonitor(true);
  speechPreview?.pause();
  speechPreview = null;
});

function advanceEditor() {
  if (!state.editor.playing) return;
  const video = $("#editor-video");
  const stamp = performance.now();
  if (!video || video.readyState < 2 || video.seeking) {
    editorTick = stamp;
    return;
  }
  const at = Math.min(timeline.duration, timeline.at + (stamp - editorTick) * state.editor.speed / 1000);
  editorTick = stamp;
  const source = timeline.videoAt(at);
  if (timeline.videoClipAt(at)?.stillId) video.pause();
  else {
    if (Math.abs(video.currentTime - source) > .2) video.currentTime = source;
    if (video.paused && at < timeline.duration) playMedia("editor", video);
  }
  updateTime("editor", at);
  if (at >= timeline.duration) listenPlayer("editor", false);
}

document.addEventListener("pointerdown", event => {
  const item = event.target.closest("[data-caption-track]");
  const track = item && timeline.tracks.find(track => track.id === item.dataset.captionTrack);
  if (event.button !== 0 || !track || track.locked) return;
  captionDrag = { item, track, bounds: item.parentElement.getBoundingClientRect() };
  item.setPointerCapture(event.pointerId);
});
document.addEventListener("pointermove", event => {
  if (!captionDrag) return;
  const { item, track, bounds } = captionDrag;
  track.x = clamp((event.clientX - bounds.left) / bounds.width, .01, .99);
  track.y = clamp((event.clientY - bounds.top) / bounds.height, .01, .99);
  item.style.left = 100 * track.x + "%";
  item.style.top = 100 * track.y + "%";
});
for (const type of ["pointerup", "pointercancel"]) document.addEventListener(type, () => {
  if (!captionDrag) return;
  captionDrag = null;
  queueEditorAutosave();
});

function reportNativeError(error) {
  toast(error?.message || String(error));
}

function registerAsset(incoming) {
  if (!incoming?.id) return null;
  const existing = assets.find(asset => asset.id === incoming.id);
  if (existing) {
    const { duration, frameRate, ...info } = incoming;
    return Object.assign(existing, info, duration > 0 ? { duration, frameRate } : {});
  }
  const asset = { ...incoming, thumbnails: [] };
  assets.push(asset);
  return asset;
}

function acceptLibrary(entries) {
  for (const item of entries ?? []) registerAsset(item);
  if (!state.asset) state.asset = assets[0]?.id || "";
  if (state.page === "review") {
    const list = $(".library-list");
    if (list) list.innerHTML = Views.libraryItems();
  }
}

async function reviewMedia(id, at = 0) {
  const asset = assets.find(item => item.id === id);
  if (!asset) return;
  const requestedTime = Number(at);
  const start = Number.isFinite(requestedTime) ? Math.max(0, requestedTime) : 0;
  state.asset = asset.id;
  state.review.at = asset.duration > 0 ? Math.min(start, asset.duration) : start;
  state.review.playing = false;
  navigate("review");
  await refreshReviewSelection();
}

function refreshReviewSelection() {
  const asset = Views.currentAsset();
  const id = asset.id;
  if (!id) return;
  if (asset.codec) { prepareThumbnails(asset); return; }
  if (reviewProbe?.id === id) return reviewProbe.promise;
  const selection = ++latestReviewSelection;
  const promise = Native.rpc("inspect-video", { id }).then(incoming => {
    if (!incoming || selection !== latestReviewSelection || state.page !== "review" || state.asset !== id) return;
    const marks = asset.marks;
    registerAsset({ ...incoming, marks });
    state.review.at = clamp(state.review.at, 0, asset.duration);
    refreshReviewMetadata();
    prepareThumbnails(asset);
  }).catch(error => {
    if (selection === latestReviewSelection && state.page === "review" && state.asset === id) reportNativeError(error);
  }).finally(() => {
    if (reviewProbe?.promise === promise) reviewProbe = undefined;
  });
  reviewProbe = { id, promise };
  return promise;
}

function prepareThumbnails(asset) {
  if (!asset?.src || !asset.duration || asset.thumbnails.length || thumbnailJob?.asset === asset) return;
  thumbnailJob?.controller.abort();
  const controller = new AbortController();
  const request = { asset, controller };
  thumbnailJob = request;
  const media = document.createElement("video");
  media.crossOrigin = "anonymous";
  media.preload = "auto";
  media.muted = true;
  const release = () => {
    if (thumbnailJob === request) thumbnailJob = undefined;
    media.removeAttribute("src");
    media.load();
    controller.abort();
  };
  const timeout = setTimeout(release, 4000);
  controller.signal.addEventListener("abort", () => {
    clearTimeout(timeout);
    if (media.hasAttribute("src")) release();
  }, { once: true });
  media.addEventListener("loadedmetadata", () => {
    clearTimeout(timeout);
    collectThumbnails(media, asset, controller.signal).catch(() => {}).finally(release);
  }, { once: true, signal: controller.signal });
  media.addEventListener("error", release, { once: true, signal: controller.signal });
  media.src = asset.playback || asset.src;
}

async function compatibilityPreview(kind) {
  const asset = kind === "review" ? Views.currentAsset() : Views.editorAsset();
  if (!asset?.id || previewing.has(asset.id)) return;
  if (asset.playback) {
    state[kind].playing = false;
    updatePlayButtons(kind);
    toast("无法播放这段视频。");
    return;
  }
  previewing.add(asset.id);
  toast("正在准备兼容预览…");
  try {
    const src = await Native.rpc("browser-video", { id: asset.mediaId || asset.id });
    if (src) {
      asset.playback = src;
      if (thumbnailJob?.asset === asset) thumbnailJob.controller.abort();
      if (state.page === kind && (kind === "review" ? Views.currentAsset() : Views.editorAsset()).id === asset.id) {
        render();
        prepareThumbnails(asset);
      }
    }
  } catch (error) {
    if (state.page === kind && (kind === "review" ? Views.currentAsset() : Views.editorAsset()).id === asset.id) {
      state[kind].playing = false;
      updatePlayButtons(kind);
      reportNativeError(error);
    }
  } finally { previewing.delete(asset.id); }
}

function setTargets(targets) {
  for (const [type, entries] of Object.entries(targets)) sourceCatalog[type].targets = entries;
  for (const scene of state.scenes) {
    for (const item of scene.sources) {
      if (!targets[item.type] || item.device == null) continue;
      item.target = targets[item.type].findIndex(([, device, monitorDevice]) =>
        item.monitorDevice ? monitorDevice === item.monitorDevice : String(device) === String(item.device));
      if (item.type === "screen" && item.target >= 0) {
        const [, device, monitorDevice] = targets.screen[item.target];
        item.device = device;
        item.monitorDevice = monitorDevice;
      }
    }
  }
}

function settingSource(from, config = {}) {
  const type = from.source || "screen";
  const device = from[type === "screen" ? "monitor" : type];
  const targets = sourceCatalog[type]?.targets ?? [];
  const target = type === "screen" && from.monitorDevice ? targets.findIndex(([, , monitorDevice]) => monitorDevice === from.monitorDevice) :
    Math.max(0, targets.findIndex(([, value]) => String(value) === String(device)));
  const item = { id: from.id || crypto.randomUUID(), type, device: device ?? sourceCatalog[type]?.targets[target]?.[1],
    target, visible: from.visible !== false, label: from.label || "",
    cursor: from.cursor !== false, clicks: from.clicks ?? config.clicks ?? false,
    screenColor: from.screenColor ?? config.screenColor, screenWhiteNits: from.screenWhiteNits ?? config.screenWhiteNits,
    screenExposureEV: from.screenExposureEV ?? config.screenExposureEV };
  if (type === "screen") item.monitorDevice = from.monitorDevice || targets[target]?.[2];
  if (type === "image") Object.assign(item, { file: from.image, label: from.label || from.image?.split(/[\\/]/).pop() });
  if (type === "region") Object.assign(item, { region: [from.x, from.y, from.rw, from.rh], label: from.label || from.rw + " × " + from.rh });
  if (type === "camera") Object.assign(item, { pipMirror: from.pipMirror ?? (config.cameraMirror !== false),
    pipShape: from.pipShape ?? (config.cameraShape === "circle" ? "圆形" : config.cameraShape === "rectangle" ? "矩形" : "圆角"),
    pipSize: from.pipSize ?? (config.cameraRect?.width * 100 || 23), pipIsolate: from.cameraTrack ?? config.cameraTrack,
    pipBorder: from.pipBorder ?? (config.cameraBorder !== "#00000000"),
    pipX: from.pipX ?? config.cameraRect?.x, pipY: from.pipY ?? config.cameraRect?.y });
  return item;
}

function commitScenes() {
  const scenes = state.scenes.map(scene => ({ ...scene, sources: scene.sources.map(item => ({ ...item,
    label: targetFor(item)[0], showCursor: item.cursor, cameraTrack: item.pipIsolate,
  })) }));
  Native.tell("scene", { scene: state.scene, scenes });
}

function channelItem(item, enabled = true) {
  mixerSources[item.id] = { name: item.name, icon: item.loopback ? "volume" : "mic",
    peak: 0, db: "−∞ dB", optional: true };
  if (enabled) state.audio[item.id] = { volume: Math.round((item.gain ?? 1) * 100), muted: Boolean(item.muted), denoise: Boolean(item.denoise) };
}

function queueNativeMixer() {
  clearTimeout(mixerTimer);
  mixerTimer = setTimeout(() => {
    Native.tell("mixer", { ...state.audio, mic: { ...state.audio.mic, denoise: state.settings.denoise } });
  }, 95);
}

function updateNativeField(field, value) {
  if (field === "capture-width" || field === "capture-height" || (field === "resolution" && Number(value) === -1)) {
    const s = state.settings;
    const even = (n, min, max) => clamp(Math.floor(n / 2) * 2, min, max);
    s.resolution = -1;
    s.width = even((field === "capture-width" ? Number(value) : s.width) || 1920, 160, 7680);
    s.height = even((field === "capture-height" ? Number(value) : s.height) || 1080, 120, 4320);
    const width = $("[data-field='capture-width']");
    const height = $("[data-field='capture-height']");
    if (!width || !height) render();
    else { width.value = s.width; height.value = s.height; }
    Native.rpc("preferences", { customSize: [s.width, s.height] }).then(bitrate => {
      s.bitrate = bitrate;
      if ($("#bitrate")) $("#bitrate").value = bitrate;
    }, reportNativeError);
    return true;
  }
  if (field === "language") {
    Native.rpc("language", { language: value }).catch(reportNativeError);
    return true;
  }
  if (field === "denoise") {
    state.settings.denoise = Boolean(value);
    queueNativeMixer();
    return true;
  }
  const key = nativeSettings.get(field);
  if (!key) return false;
  const numeric = ["resolution", "fps", "bitrate", "countdown"].includes(field);
  state.settings[key] = numeric ? Number(value) : value;
  if (field === "resolution") {
    const size = ({ 720: [1280, 720], 1080: [1920, 1080], 1440: [2560, 1440], 2160: [3840, 2160] })[value];
    if (size) [state.settings.width, state.settings.height] = size;
    render();
  }
  if (field === "bitrate") state.settings.customBitrate = true;
  Native.rpc("preferences", { [key]: state.settings[key],
    ...(field === "bitrate" ? { customBitrate: true } : {}) }).then(bitrate => {
    state.settings.bitrate = bitrate;
    const input = $("#bitrate");
    if (input && document.activeElement !== input) input.value = bitrate;
    $("#statusbar").innerHTML = Views.status();
  }).catch(reportNativeError);
  return true;
}

function nativeSchedules(entries) {
  state.schedule = (entries || []).map(task => ({ id: task.id, name: task.name, at: Date.parse(task.startAt),
    duration: task.duration, status: task.state, scene: task.source || "" })).filter(task => Number.isFinite(task.at));
  scheduleDialog();
}

function addNativeSchedule(name, at, duration, scene) {
  Native.rpc("schedule-add", { name, at: new Date(at).toISOString(), duration,
    scene: state.scenes.findIndex(item => item.name === scene) }).then(nativeSchedules, reportNativeError);
}

function addCueLines(track, lines) {
  const end = timeline.duration;
  const sorted = [...lines].sort((a, b) => a.at - b.at);
  track.clips = sorted.map((clip, i) => ({ ...clip,
    length: Math.min(clip.length, end - clip.at, (sorted[i + 1]?.at ?? end) - clip.at),
  })).filter(clip => clip.length >= .001 && clip.at >= 0);
}

async function recognizeSpeech() {
  const track = locateSubtitleTrack();
  const available = timeline.tracks.filter(item => item.kind === "audio" && (item.stream != null || item.resource));
  const index = available.findIndex(item => item.id === state.editor.asrTrack);
  if (!hasExplicitEditorProject() || !track || track.locked || index < 0 || speechWorking) return;
  const project = activeEditorProjectId();
  speechWorking = true;
  toast("正在识别所选音轨…");
  try {
    await flushEditorChanges();
    const lines = await Native.rpc("recognize", { track: index, lang: state.editor.asrLanguage === "英语" ? "en" : "zh" });
    if (activeEditorProjectId() !== project || !lines?.length) return;
    timeline.remember();
    addCueLines(track, lines);
    timelineChange();
  } catch (error) { reportNativeError(error); }
  finally { speechWorking = false; }
}

async function translateCues(id, review = false) {
  const track = locateSubtitleTrack();
  if (!track || track.locked || speechWorking) return;
  const clips = track.clips.filter(clip => !id || clip.id === id)
    .map(({ id, name, translation }) => ({ id, name, translation }));
  if (!clips.length) return;
  const project = activeEditorProjectId();
  const trackIndex = timeline.tracks.filter(item => item.kind === "subtitle").indexOf(track);
  const lang = state.editor.direction === "英 → 中" ? "zh" : "en";
  const glossary = state.editor.glossary;
  const stillEditingTrack = () => project === activeEditorProjectId() && timeline.tracks.includes(track) && !track.locked;
  speechWorking = true;
  toast(review ? "正在校对…" : "正在翻译…");
  try {
    await flushEditorChanges();
    if (!stillEditingTrack()) return;
    const results = await Native.rpc(review ? "proofread" : "translate", {
      track: trackIndex, lang, glossary, lines: clips.map(clip => clip.name),
    });
    if (!stillEditingTrack()) return;
    const byId = new Map(track.clips.map(clip => [clip.id, clip]));
    const replacements = [];
    for (const { index, translation } of review ? results : results.map((translation, index) => ({ index, translation }))) {
      const original = clips[index];
      const current = byId.get(original?.id);
      if (current && translation && translation !== current.translation &&
          current.name === original.name && current.translation === original.translation)
        replacements.push({ current, translation });
    }
    if (!replacements.length) {
      if (review && !results.length) toast("校对完成，译文无需修改。");
      return;
    }
    timeline.remember();
    for (const { current, translation } of replacements) current.translation = translation;
    if (state.page === "editor") timelineChange();
    else await flushEditorChanges();
    toast(review ? "校对完成。" : "翻译完成。");
  } catch (error) { reportNativeError(error); }
  finally { speechWorking = false; }
}

function chapterNarrationSections() {
  const marks = timeline.markers.filter(mark => mark.at >= 0 && mark.at < timeline.duration)
    .slice().sort((a, b) => a.at - b.at);
  if (marks[0]?.at !== 0) marks.unshift({ at: 0, name: marks.length ? "开头" : "全片" });
  return marks.map((mark, index) => ({
    at: mark.at, uid: mark.at.toFixed(3), title: mark.name, length: (marks[index + 1]?.at ?? timeline.duration) - mark.at,
  })).filter(section => section.length >= .1);
}

function parseChapterText(text, sections, at) {
  const rows = text.replace(/\r\n?/g, "\n").split("\n");
  const current = sections.find(section => section.at <= at && at < section.at + section.length) ?? sections.at(-1);
  if (!current) throw new Error("请先准备可配音的章节。");
  if (!rows.some(row => /^\s*##\s+\S/.test(row))) return { [current.uid]: text.trim() };
  const scripts = {};
  let target, body = [];
  for (const row of rows) {
    const heading = row.match(/^\s*##\s+(\S.*?)\s*$/)?.[1];
    if (!heading) {
      if (!target && row.trim()) throw new Error("多章稿件请以「## 章节标题」开头。");
      body.push(row);
      continue;
    }
    if (target) scripts[target.uid] = body.join("\n").trim();
    body = [];
    const titled = sections.filter(section => section.title.toLowerCase() === heading.toLowerCase());
    const numbered = heading.match(/^([1-9]\d*)(?:[.、]\s*(.*))?$/);
    const indexed = numbered && sections[Number(numbered[1]) - 1];
    target = titled.length === 1 ? titled[0] :
      indexed && (!numbered[2] || indexed.title.toLowerCase() === numbered[2].toLowerCase()) ? indexed : null;
    if (!target || Object.hasOwn(scripts, target.uid)) throw new Error("找不到唯一章节「" + heading + "」。");
  }
  if (target) scripts[target.uid] = body.join("\n").trim();
  if (!Object.values(scripts).some(Boolean)) throw new Error("稿件没有可导入的章节文本。");
  return scripts;
}

async function importChapterText() {
  if (!hasExplicitEditorProject()) { toast("请先打开剪辑工程。"); return; }
  const dub = state.editor.dub;
  try {
    const text = await Native.rpc("chapter-script-import");
    if (!text || dub !== state.editor.dub) return;
    const imported = parseChapterText(text, chapterNarrationSections(), timeline.at);
    const scripts = { ...dub.chapterScripts, ...imported };
    const count = Object.values(scripts).reduce((total, item) => total + item.length, 0);
    if (count > 60000) throw new Error("同一工程的章节稿件最多 60000 字。");
    dub.chapterScripts = scripts;
    timelineChange();
    toast("已导入 " + Object.keys(imported).length + " 个章节");
  } catch (error) { reportNativeError(error); }
}

function narrationLines() {
  const dub = state.editor.dub;
  if (dub.mode === "sentences") return (locateSubtitleTrack()?.clips || []).map((clip, i) => ({
    at: clip.at, length: clip.length, uid: String(i), rules: dub.rules[i] || "",
    text: dub.scripts[i] ?? (dub.textType[i] === "使用译文" ? clip.translation : clip.name),
  }));
  if (dub.mode === "chapter") return chapterNarrationSections().map(section => ({
    ...section, text: dub.chapterScripts[section.uid], rules: dub.chapterRules[section.uid] || "",
  }));
  return [{ at: timeline.at, length: timeline.duration - timeline.at, uid: "", text: dub.narration, rules: dub.narrationRule }];
}

function onlineNarrationEngine(dub) {
  return state.ttsServices.find(({id}) => id === dub.engine)?.id || state.ttsServices[0]?.id || "";
}

function narrationSignature(dub, lines) {
  return JSON.stringify([dub.mode, dub.service, dub.service === "online" ? onlineNarrationEngine(dub) : dub.voice, dub.speed, dub.volume,
    lines.map(({ at, length, text, rules }) => [at, length, text, rules])]);
}

async function produceNarration(index, audition = false) {
  if (!hasExplicitEditorProject() || speechWorking) return;
  const dub = state.editor.dub;
  const engine = dub.service === "online" ? onlineNarrationEngine(dub) : "";
  if (dub.service === "online" && !engine) { toast("请先配置并选择在线配音服务。"); return; }
  let lines = narrationLines();
  if (audition) {
    const sample = index != null ? lines.find(line => line.uid === index) : { text: "你好，这里是 WPS Recorder 配音试听。" };
    lines = sample ? [{ ...sample, text: sample.text?.slice(0, 64), at: 0, length: 12 }] : [];
  }
  lines = lines.filter(line => line.length >= .1 && line.text?.trim());
  if (!lines.length) { toast("请先填写配音文本，并保留足够的时长。"); return; }
  const signature = narrationSignature(dub, lines);
  speechWorking = true;
  try {
    await flushEditorChanges();
    const file = await Native.rpc("dubbing", { lines, audition, mode: dub.mode, duration: audition ? lines[0].length : timeline.duration,
      service: dub.service, engine, voice: dub.voice, speed: dub.speed, volume: dub.volume });
    if (dub !== state.editor.dub || !file?.id) return;
    if (audition) {
      speechPreview?.pause();
      speechPreview = new Audio(file.url);
      await speechPreview.play();
    } else {
      const slots = lines.map(({ at, length }) => ({ at, length }));
      dub.file = { ...file, signature, mode: dub.mode, slots, speech: file.speech?.length ? file.speech : slots };
      toast("配音已生成");
    }
  } catch (error) { reportNativeError(error); }
  finally { speechWorking = false; }
}

const toggleMute = actions["mixer-mute"];
const toggleDenoise = actions.denoise;
const removeAudio = actions["mixer-remove"];
Object.assign(actions, {
  minimize: () => Native.tell("window", { action: "minimize" }),
  "close-app": async () => {
    if (requestingClose) return;
    requestingClose = true;
    try {
      let draftSyncFailed = false;
      try { await flushEditorChanges(); } catch { draftSyncFailed = true; }
      await Native.rpc("window", { action: "close", draftSyncFailed });
    } catch (error) { reportNativeError(error); }
    finally { requestingClose = false; }
  },
  "float-toggle": () => Native.tell("floating", { active: !state.telemetry.floating }),
  "minimal-recorder": async () => {
    try {
      await flushEditorChanges();
      Native.tell("minimal-recorder");
    } catch (error) { reportNativeError(error); }
  },
  "subtitle-style": openSubtitleStyle,
  "chapter-script-import": importChapterText,
  "pause-record": () => Native.tell("pause"),
  "record-mark": () => Native.tell("record-mark"),
  screenshot: () => Native.tell("screenshot"),
  annotate: () => Native.tell("annotate"),
  "choose-output-path": async () => {
    const name = await Native.rpc("choose-directory", { save: true }).catch(reportNativeError);
    if (name) updateOutputFolder(name);
  },
  "open-directory": () => Native.tell("open-directory"),
  "native-models": initial => settingsDialog("models", initial),
  "reset-rate": async () => {
    try {
      state.settings.bitrate = await Native.rpc("recommended");
      state.settings.customBitrate = false;
      const input = $("#bitrate");
      if (input) input.value = state.settings.bitrate;
      $("#statusbar").innerHTML = Views.status();
    } catch (error) { reportNativeError(error); }
  },
  "open-local": async () => {
    const asset = registerAsset(await Native.rpc("choose-video").catch(reportNativeError));
    if (asset) {
      state.review.filter = "all";
      state.review.query = "";
      await reviewMedia(asset.id);
    }
  },
  "review-select": id => reviewMedia(id),
  "remove-video": () => {
    const asset = Views.currentAsset();
    if (!asset.id) return;
    showDialog("remove-video", "删除录制", `<p>${esc(UI.text("将此视频移到回收站？"))}</p><p>${esc(asset.name)}</p>`,
      b("confirm-remove-video", "移到回收站", "trash", { variant: "primary", value: asset.id }));
  },
  "confirm-remove-video": async id => {
    if (dialogName !== "remove-video") return;
    dialog.close();
    const video = $("#review-video");
    if (video?.dataset.assetId === id) {
      thumbnailJob?.controller.abort();
      state.review.playing = false;
      video.pause();
      video.removeAttribute("src");
      video.load();
    }
    try {
      const result = await Native.rpc("remove-video", { id, confirmed: true });
      if (!result?.removed) return;
      const at = assets.findIndex(asset => asset.id === id);
      if (at >= 0) assets.splice(at, 1);
      if (state.asset === id) {
        state.asset = "";
        state.review.at = 0;
      }
      acceptLibrary(result.entries);
      if (result.recoveryPath) toast(UI.text("已移到回收目录：") + result.recoveryPath);
    } catch (error) {
      reportNativeError(error);
    } finally {
      if (state.page === "review" && (!video || video.isConnected)) {
        render();
        refreshReviewSelection();
      }
    }
  },
  "reveal-media": () => Native.tell("reveal-media", { id: state.asset }),
  "recover-recording": () => Native.rpc("recover").then(acceptLibrary, reportNativeError),
  "open-editor-project": showEditorProjectPicker,
  "import-editor-project": chooseEditorProjectFile,
  save: saveNativeProject,
  "export-path": async () => {
    const draft = exportDraft;
    const path = await Native.rpc("choose-directory").catch(reportNativeError);
    if (draft !== exportDraft || !path || !$("#export-folder", dialog)) return;
    draft.folder = path;
    const control = $("#export-folder", dialog);
    control.value = control.title = path;
  },
  "mixer-picker": async () => {
    const endpoints = await Native.rpc("audio-devices").catch(reportNativeError);
    for (const endpoint of endpoints || []) channelItem(endpoint, false);
    if (!availableMixerSources().length) { toast("没有其他可用音频设备。"); return; }
    addMixerSourceDialog();
  },
  "mixer-add": async () => {
    const id = $('input[name="mixer-source"]:checked', dialog)?.value;
    if (!id) return;
    const entry = await Native.rpc("add-audio", { id }).catch(reportNativeError);
    if (entry) { channelItem(entry); dialog.close(); renderMixer({ reveal: true }); }
  },
  "mixer-mute": id => { toggleMute(id); queueNativeMixer(); },
  "mixer-remove": id => { removeAudio(id); queueNativeMixer(); },
  denoise: id => { toggleDenoise(id); queueNativeMixer(); },
  schedule: () => Native.rpc("schedule-list").then(nativeSchedules, reportNativeError),
  "remove-schedule": index => Native.rpc("schedule-remove", { id: state.schedule[Number(index)]?.id })
    .then(nativeSchedules, reportNativeError),
  "still-import": async () => {
    const file = await Native.rpc("choose-image").catch(reportNativeError);
    if (file?.id && hasExplicitEditorProject()) {
      state.editor.stills.unshift({ id: file.id, title: file.name, image: file.url });
      timelineChange();
    }
  },
  "add-audio": async () => {
    if (!hasExplicitEditorProject()) { toast("请先打开工程。"); return; }
    const project = activeEditorProjectId();
    const at = timeline.at;
    const file = await Native.rpc("choose-audio").catch(reportNativeError);
    if (!file?.id || activeEditorProjectId() !== project) return;
    if (timeline.duration - at < .01) { toast("请把播放头移到工程结束前。"); return; }
    const id = timeline.addTrack("audio");
    const track = timeline.tracks.find(item => item.id === id);
    Object.assign(track, { name: file.name, resource: file.id, src: file.url, gain: 1 });
    track.clips = [{ id: crypto.randomUUID(), at, length: Math.min(file.duration || timeline.duration, timeline.duration - at),
      name: file.name, source: 0 }];
    timelineChange();
  },
  "srt-import": async () => {
    if (!hasExplicitEditorProject()) return;
    const track = locateSubtitleTrack();
    if (!track || track.locked) return;
    const lines = await Native.rpc("srt-import").catch(reportNativeError);
    if (!lines?.length) return;
    timeline.remember();
    addCueLines(track, lines);
    timelineChange();
  },
  recognize: recognizeSpeech,
  translate: () => translateCues(),
  proofread: () => translateCues(undefined, true),
  retranslate: id => translateCues(id),
  audition: index => produceNarration(index, true),
  "dub-original": time => { seekPlayer("editor", Number(time)); listenPlayer("editor", true); },
  "dub-generate": index => produceNarration(index),
  "dub-add": () => {
    const dub = state.editor.dub;
    const file = dub.file;
    if (!file?.id) { toast("请先生成配音。"); return; }
    const slots = narrationLines();
    if (dub.mode === "custom" && slots.length && file.slots[0]) Object.assign(slots[0], file.slots[0]);
    const lines = slots.filter(line => line.length >= .1 && line.text?.trim());
    if (narrationSignature(dub, lines) !== file.signature) { toast("请重新生成已修改的配音。"); return; }
    const chapterDubbing = file.mode === "chapter";
    let track = timeline.tracks.find(item => item.resource === file.id || (chapterDubbing && item.chapterDubbing));
    if (!track) {
      const id = timeline.addTrack("audio");
      track = timeline.tracks.find(item => item.id === id);
    } else if (track.locked) { toast("请先解锁配音轨。"); return; }
    const name = chapterDubbing ? "章节旁白" : file.mode === "custom" ? "自填旁白" : "字幕配音";
    monitorTracks.get(track.id)?.player?.pause();
    monitorTracks.delete(track.id);
    Object.assign(track, { name, resource: file.id, src: file.url, gain: 1, stream: -1,
      chapterDubbing, speech: file.speech, originalGain: dub.duck ? .35 : 1 });
    track.clips = file.speech.map(({ at, length }) => ({
      id: crypto.randomUUID(), at, length: Math.min(timeline.duration - at, length), name, source: at,
    })).filter(clip => clip.length >= .1);
    timelineChange();
  },
  "srt-export": () => flushEditorChanges().then(() => Native.rpc("srt-export", {
    track: timeline.tracks.filter(item => item.kind === "subtitle").indexOf(locateSubtitleTrack())
  })).then(path => { if (path) toast("字幕已导出"); }, reportNativeError),
  "export-select-srt": () => {
    const draft = exportDraft;
    if (!draft || draft.project || draft.copyStreams || !supportsExportSrt(draft.format)) return;
    return Native.rpc("select-srt").then(file => {
      if (draft !== exportDraft || !dialog.open || dialogName !== "export" || draft.copyStreams ||
          !supportsExportSrt(draft.format) || !file?.id) return;
      draft.subtitleId = file.id;
      draft.subtitleFile = file.name;
      exportDialog("subtitleFile");
    }, reportNativeError);
  },
  help: () => settingsDialog("system"),
  "native-guide": () => Native.tell("open-guide"),
});

let lastPreviewRetry = -Infinity;
function updatePreviewNotice(media, notice) {
  media.hidden = Boolean(notice);
  const label = $("#capture-notice");
  if (label) {
    label.hidden = !notice;
    if (notice) label.textContent = UI.text(notice);
  }
}

function updateCameraPreviewStatus() {
  const outline = $(".pip-placement");
  if (outline) outline.hidden = !state.telemetry.cameraReady;
  const label = $("#capture-camera-notice");
  if (label) {
    label.hidden = !state.liveFrame || Boolean(state.telemetry.cameraReady);
    label.firstElementChild.textContent = cameraPreviewNotice();
    label.title = state.telemetry.cameraError || "";
  }
}

document.addEventListener("load", event => {
  const image = event.target;
  if (image.id === "capture-image" && image.naturalWidth > 0) updatePreviewNotice(image, "");
}, true);
document.addEventListener("error", event => {
  if (event.target.id !== "capture-image" || !event.target.hasAttribute("src")) return;
  updatePreviewNotice(event.target, "正在重新连接画面源…");
  if (performance.now() - lastPreviewRetry < 1000) return;
  lastPreviewRetry = performance.now();
  Native.tell("preview-refresh");
}, true);

Native.on("frame", frame => {
  const previous = state.liveFrame;
  state.liveFrame = frame?.src ? frame : null;
  state.previewNotice = frame?.notice || "";
  if (Boolean(previous) !== Boolean(state.liveFrame)) updateCameraPreviewStatus();
  const media = $("#capture-image");
  if (!media) return;
  if (!frame?.src) {
    media.removeAttribute("src");
    updatePreviewNotice(media, state.previewNotice || "正在连接画面源…");
    return;
  }
  media.src = frame.src;
  const dimensionChanged = !previous || previous.width !== frame.width || previous.height !== frame.height;
  const monitor = $("#capture-frame");
  if (dimensionChanged && monitor) applyZoom();
});
Native.on("request-close", () => actions["close-app"]());
Native.on("request-minimal", () => actions["minimal-recorder"]());
Native.on("status", data => {
  const previous = state.record.phase;
  const maximized = state.telemetry.maximized;
  const cameraChanged = state.telemetry.cameraReady !== data.cameraReady || state.telemetry.cameraError !== data.cameraError ||
    state.telemetry.cameraPermissionRestricted !== data.cameraPermissionRestricted;
  state.telemetry = data;
  Object.assign(state.record, { phase: data.phase, seconds: data.seconds, countdown: data.countdown });
  if (previous !== data.phase) {
    renderRecordState();
    if (["recording", "paused", "saving"].includes(previous) && data.phase === "idle")
      Native.rpc("library").then(acceptLibrary, reportNativeError);
  } else {
    $$("[data-clock]").forEach(clock => { clock.textContent = time(data.seconds, true); });
    const countdown = $("#countdown-left");
    if (countdown) countdown.textContent = data.countdown;
    $("#statusbar").innerHTML = Views.status();
  }
  if (maximized !== data.maximized) $("#titlebar").innerHTML = Views.titlebar();
  if (cameraChanged) updateCameraPreviewStatus();
  if (data.message && data.message !== displayedMessage) toast(data.message);
  displayedMessage = data.message;
});
Native.on("levels", levels => {
  for (const [id, value] of Object.entries(levels)) {
    if (!mixerSources[id]) continue;
    const percent = Math.min(100, value * 100);
    const db = value > 0 ? (20 * Math.log10(value)).toFixed(1) + " dB" : "−∞ dB";
    Object.assign(mixerSources[id], { peak: percent, db });
    const meter = $("[data-meter='" + CSS.escape(id) + "']");
    meter?.style.setProperty("--meter-level", percent + "%");
    meter?.setAttribute("aria-valuenow", Math.round(percent));
    const text = $("[data-db='" + CSS.escape(id) + "']");
    if (text) text.textContent = state.audio[id]?.muted ? UI.text("静音") : db;
  }
});
Native.on("library", acceptLibrary);
Native.on("library-metadata", ({ id, duration, poster }) => {
  const asset = assets.find(entry => entry.id === id);
  if (!asset) return;
  if (!asset.duration && duration > 0) asset.duration = duration;
  if (poster && !asset.poster) asset.poster = poster;
  const thumb = $(`.library-list [data-value="${CSS.escape(id)}"] .media-thumb`);
  if (thumb) {
    $("time", thumb).textContent = asset.duration > 0 ? time(asset.duration) : "—";
    const placeholder = $(".source-icon", thumb);
    if (placeholder && poster) {
      const image = document.createElement("img");
      image.alt = "";
      image.src = poster;
      placeholder.replaceWith(image);
    }
  }
  if (state.page === "review" && state.asset === id) refreshReviewMetadata();
});
Native.on("navigate", navigate);
Native.on("annotation", active => {
  state.annotation.open = active;
  $("[data-action='annotate']")?.setAttribute("aria-pressed", active);
});
Native.on("floating", active => {
  state.telemetry.floating = active;
  $("#titlebar").innerHTML = Views.titlebar();
});

document.addEventListener("pointerdown", event => {
  if (event.button !== 0 || !event.target.closest("#titlebar") ||
      event.target.closest("button, summary, a, input, select, .menu-popover")) return;
  Native.tell("window", { action: "move" });
});

function syncToolbarTheme() {
  const light = ThemeManager.current().scheme === "light";
  const data = { light };
  if (light) {
    const css = getComputedStyle(document.documentElement);
    const tokens = { head: "--head", accent: "--blue", border: "--theme-stroke-strong",
      text: "--text", field: "--field", divider: "--soft-stroke", hover: "--hover",
      selected: "--blue-dark", selectedBorder: "--theme-active-stroke", muted: "--muted", record: "--red" };
    for (const [name, token] of Object.entries(tokens)) data[name] = css.getPropertyValue(token).trim();
  }
  Native.tell("toolbar-theme", data);
}
document.addEventListener("themechange", syncToolbarTheme);

initializeEditorProjects();
render();
const startupNavigationEpoch = workspaceNavigationEpoch;
Native.rpc("init").then(async boot => {
  syncToolbarTheme();
  const cfg = boot.configuration;
  Object.assign(state.settings, boot.settings);
  UI.setLanguage(state.settings.language, boot.messages);
  state.hotkeys = boot.hotkeys || [];
  state.hotkeyLabels = boot.hotkeyLabels || [];
  state.version = boot.version;
  setTargets(boot.targets);
  state.scenes = boot.scenes.map(row => {
    const config = row.config;
    const sources = row.sources?.length ? row.sources.map(from => settingSource(from, config)) :
      [settingSource(config, config)];
    if (config.cameraEnabled && !sources.some(source => source.type === "camera"))
      sources.push(settingSource({ source: "camera", camera: config.camera }, config));
    const scene = makeScene(row.name, sources);
    if (sources[row.sourceIndex]) scene.selected = sources[row.sourceIndex].id;
    return scene;
  });
  if (state.scenes.length === 0) state.scenes.push(makeScene("场景 1", [settingSource(cfg, cfg)]));
  state.scene = clamp(boot.scene, 0, state.scenes.length - 1);
  state.audio.mic = { volume: Math.round(cfg.micGain * 100), muted: !["mix", "mic"].includes(cfg.audio) };
  state.audio.system = { volume: Math.round(cfg.systemGain * 100), muted: !["mix", "system"].includes(cfg.audio) };
  for (const item of boot.audio) channelItem(item);
  acceptLibrary(boot.assets);
  state.modelReady = boot.audio8;
  state.ttsServices = boot.ttsServices || [];
  render();
  if (boot.startupAsset) {
    const asset = registerAsset(boot.startupAsset);
    await switchEditorAsset(asset, undefined, startupNavigationEpoch);
  }
}).catch(reportNativeError);
