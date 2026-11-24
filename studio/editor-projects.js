"use strict";

const {
  listEditorProjects, activeEditorProjectId, editorProjectAsset, editorProjectTimeline, hasExplicitEditorProject,
  openEditorProject, showEditorProjectPicker, chooseEditorProjectFile, switchEditorAsset,
  initializeEditorProjects, queueEditorAutosave, persistEditorDraft, registerEditorStillFile, registerEditorVideoFile,
  flushEditorChanges, saveNativeProject, openNativeProject,
} = (() => {
  const storageKey = "wps-recorder-studio-projects";
  const projects = new Map();
  let draft;
  let defaults;
  let timer;
  let savedContent = "";
  let sentContent = "";
  let inFlight = Promise.resolve();
  let projectSwitch = Promise.resolve();

  function initializeEditorProjects() {
    defaults = structuredClone(state.editor);
    try {
      for (const item of JSON.parse(localStorage.getItem(storageKey) || "[]")) {
        if (typeof item?.id === "string" && typeof item?.path === "string") projects.set(item.id, item);
      }
    } catch {}
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "hidden") persistEditorDraft();
    });
  }

  function listEditorProjects() {
    const known = [...projects.values()];
    if (draft && !projects.has(draft.id)) known.unshift(draft);
    return known;
  }

  function activeEditorProjectId() { return draft?.id || ""; }
  function hasExplicitEditorProject() { return Boolean(draft); }
  function editorProjectAsset(id = activeEditorProjectId()) {
    if (!id) return undefined;
    const project = draft?.id === id ? draft : projects.get(id);
    return assets.find(asset => asset.id === (project?.assetId || id));
  }
  function editorProjectTimeline(id = activeEditorProjectId()) {
    return id && id === draft?.id ? timeline.snapshot() : null;
  }

  function editorChanges() {
    const settings = state.editor;
    const crop = currentEditorCrop().map(Math.round);
    const snapshot = timeline.snapshot();
    snapshot.tracks = snapshot.tracks.map(track => ({
      ...track, clips: track.clips.map(clip => {
        if (!clip.stillId) return clip;
        const still = editorStillItem(clip.stillId);
        return { ...clip, resource: still?.resource || still?.id || "" };
      }),
    }));
    const style = { subtitleSize: settings.subtitleSize, subtitleColor: settings.subtitleColor,
      subtitleFont: settings.subtitleFont, subtitleOpacity: settings.subtitleOpacity };
    const chapterDrafts = Object.fromEntries(chapterNarrationSections().map(({ uid }) =>
      [uid, { text: settings.dub.chapterScripts[uid] || "", rules: settings.dub.chapterRules[uid] || "" }])
      .filter(([, draft]) => draft.text || draft.rules));
    const content = { tracks: snapshot.tracks, trackOrder: snapshot.trackOrder, markers: snapshot.markers, crop, ...style, chapterDrafts };
    return { data: { id: Views.editorAsset().id, timeline: snapshot, settings: { cropRect: crop,
      ...style, chapterDrafts } }, content: JSON.stringify(content) };
  }

  function flushEditorChanges() {
    clearTimeout(timer);
    if (!draft) return Promise.resolve();
    const change = editorChanges();
    if (sentContent === change.content) return inFlight;
    sentContent = change.content;
    inFlight = inFlight.catch(() => {}).then(() => Native.rpc("editor-change", change.data)).catch(error => {
      if (sentContent === change.content) sentContent = "";
      throw error;
    });
    return inFlight;
  }

  function persistEditorDraft() { return flushEditorChanges().catch(reportNativeError); }
  function queueEditorAutosave() {
    if (!draft || state.page !== "editor") return;
    clearTimeout(timer);
    timer = setTimeout(persistEditorDraft, 260);
  }

  function addProject(record) {
    if (!record?.path) return;
    projects.set(record.id, record);
    localStorage.setItem(storageKey, JSON.stringify([...projects.values()].slice(-120)));
  }

  async function performSwitch(data, at, navigation) {
    await flushEditorChanges();
    const previous = draft?.id;
    const result = await Native.rpc("editor-open", data);
    if (!result?.asset?.id) return false;
    const asset = registerAsset(result.asset);
    const edit = result.edit;
    const id = data.project || edit.path || asset.id;
    draft = { id, title: edit.path ? edit.path.split(/[\\/]/).pop().replace(/\.krrproj$/i, "") : asset.title,
      path: edit.path, assetId: asset.id };
    if (draft.path) addProject(draft);
    state.editorAsset = id;
    state.editor = structuredClone(defaults);
    state.editor.stills = edit.stills.map(still => ({ ...still, title: still.name, resource: still.id }));
    state.editor.subtitleSize = edit.subtitleSize;
    state.editor.subtitleColor = edit.subtitleColor;
    state.editor.subtitleFont = edit.subtitleFont || defaults.subtitleFont;
    state.editor.subtitleOpacity = edit.subtitleOpacity ?? defaults.subtitleOpacity;
    for (const [uid, script] of Object.entries(edit.chapterDrafts || {})) {
      state.editor.dub.chapterScripts[uid] = script.text;
      state.editor.dub.chapterRules[uid] = script.rules;
    }
    if (edit.crop[2] && (edit.crop[2] !== asset.width || edit.crop[3] !== asset.height)) {
      state.editor.crop = "自由";
      state.editor.cropRect = edit.crop;
    }
    timeline.reset(asset.duration, asset.id, asset.thumbnails, asset.frameRate);
    timeline.tracks = edit.tracks;
    timeline.trackOrder = edit.trackOrder || [];
    timeline.markers = edit.markers;
    timeline.selected = timeline.tracks[0].clips[0]?.id || "";
    timeline.setTime(at || 0);
    state.editor.subtitleTrack = timeline.tracks.find(track => track.kind === "subtitle")?.id || "T1";
    state.editor.asrTrack = timeline.tracks.find(track => track.kind === "audio")?.id || "";
    inFlight = Promise.resolve();
    savedContent = sentContent = editorChanges().content;
    if (navigation === workspaceNavigationEpoch) navigate("editor");
    else if (state.page === "editor") render();
    if (previous !== id) document.dispatchEvent(new CustomEvent("editorprojectchange"));
    if (state.page === "editor") prepareThumbnails(asset);
    return true;
  }

  function queueProjectSwitch(data, at, navigation) {
    projectSwitch = projectSwitch.then(() => performSwitch(data, at, navigation)).catch(error => {
      reportNativeError(error);
      return false;
    });
    return projectSwitch;
  }

  function openNativeProject(data, at, navigation = workspaceNavigationEpoch) {
    if (!draft || savedContent === editorChanges().content) return queueProjectSwitch(data, at, navigation);
    return new Promise(resolve => {
      showDialog("switch-editor-project", "切换工程", "<p>当前工程的修改会自动保存为草稿。</p>",
        '<button type="button" class="btn primary" id="apply-project-switch">保存草稿并切换</button>');
      const cancel = () => resolve(false);
      dialog.addEventListener("close", cancel, { once: true });
      $("#apply-project-switch", dialog).addEventListener("click", () => {
        dialog.removeEventListener("close", cancel);
        dialog.close();
        resolve(queueProjectSwitch(data, at, workspaceNavigationEpoch));
      }, { once: true });
    });
  }

  function switchEditorAsset(asset, at, navigation) {
    if (!asset?.id) return Promise.resolve(false);
    if (draft?.assetId === asset.id) { navigate("editor"); if (at != null) seekPlayer("editor", at); return Promise.resolve(true); }
    return openNativeProject({ id: asset.id }, at, navigation);
  }

  function openEditorProject(id, at) {
    if (id === draft?.id) { navigate("editor"); if (at != null) seekPlayer("editor", at); return; }
    const project = projects.get(id);
    if (project) return openNativeProject({ path: project.path, project: project.id }, at);
    const asset = editorProjectAsset(id);
    if (asset) return switchEditorAsset(asset, at);
  }

  function chooseEditorProjectFile() { return openNativeProject({ pick: true }); }

  function showEditorProjectPicker() {
    const rows = listEditorProjects().map(project => b("editor-media", project.title, "folder", { value: project.id, variant: "flat" })).join("");
    showDialog("open-editor-project", "打开剪辑工程", rows ? '<div class="stack">' + rows + '</div>' : "",
      b("import-editor-project", "选择工程文件…", "folder", { variant: "primary" }));
  }

  async function saveNativeProject() {
    if (!draft) { toast("请先打开工程。"); return; }
    try {
      await flushEditorChanges();
      const path = await Native.rpc("editor-save");
      if (!path) return;
      draft.path = path;
      addProject(draft);
      savedContent = editorChanges().content;
      toast("工程已保存");
    } catch (error) { reportNativeError(error); }
  }

  async function registerEditorStillFile(file) {
    const data = await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onerror = reject;
      reader.onload = () => resolve(reader.result);
      reader.readAsDataURL(file);
    });
    const result = await Native.rpc("save-still", { data, name: file.name });
    return { id: result.id, image: result.url, resource: result.id };
  }

  function registerEditorVideoFile() {
    // Video files arrive through the operating system picker and are registered by the host.
  }

  return {
    listEditorProjects, activeEditorProjectId, editorProjectAsset, editorProjectTimeline, hasExplicitEditorProject,
    openEditorProject, showEditorProjectPicker, chooseEditorProjectFile, switchEditorAsset,
    initializeEditorProjects, queueEditorAutosave, persistEditorDraft, registerEditorStillFile, registerEditorVideoFile,
    flushEditorChanges, saveNativeProject, openNativeProject,
  };
})();
