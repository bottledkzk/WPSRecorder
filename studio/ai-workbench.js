"use strict";

const AgentWorkspace = (() => {
  const { button: b, iconButton: ib, glyph: g, escape: esc, head, time, select, clamp } = UI;
  const tr = label => esc(UI.text(label));
  const storageKey = "wps-recorder-alignment-conversations-v1";
  const projectStorageKey = "wps-recorder-alignment-assistant-projects-v1";
  const workspace = "workspace";
  const defaultParams = { range: "full", target: 90, subtitles: true, markers: true, picture: true, from: 0, to: 30 };
  const modeTitles = { record: "录屏助手", edit: "剪辑助手" };
  const starters = {
    record: ["找包含打点的录像", "录制卡顿怎么排查？", "预览 1080p60 录制参数"],
    edit: ["按打点挑出重点片段", "按目标时长整理剪辑草稿", "检查当前剪辑轨道"],
  };
  let threads = load();
  let mode = "record";
  let project = "";
  const registrations = loadProjectRegistrations();
  const closedProjectGroups = new Set();
  const rightLayouts = new Map();
  const rightPages = {
    workspace: ["工作区", "folder"],
    materials: ["素材", "film"],
    preview: ["视频预览", "play"],
    terminal: ["CLI 终端", "terminal"],
  };
  const active = new Map();
  const sideThreads = new Map();
  const drafts = new Map();
  const jobs = new Map();
  const workspaceAssets = new Map();
  const workspaceLoads = new Map();
  const workspaceMediaLoads = new Map();
  const inferredWorkspaceFolders = new Map();
  const scope = (kind, projectId) => kind === "record" ? kind : kind + ":" + projectId;
  const byId = id => threads.find(thread => thread.id === id);
  const lookupAsset = id => assets.find(asset => asset.id === id) || workspaceAssets.get(id);
  const projects = () => listEditorProjects();
  const currentProject = () => activeEditorProjectId();
  const editorProject = id => projects().find(item => item.id === id);
  const projectTitle = id => registrations.get(id)?.title ?? (id === workspace ? "工作区" : editorProject(id)?.title ?? "未载入工程");
  const projectEditorId = id => registrations.get(id)?.editorId ?? (registrations.get(id)?.project ? id : editorProject(id)?.id);
  const asAssetId = id => editorProject(projectEditorId(id))?.assetId;
  const projectWorkspace = id => registrations.get(id)?.folders ?? [];
  const projectWorkspaceReplaced = id => Boolean(registrations.get(id)?.replaced);

  function loadProjectRegistrations() {
    try {
      const saved = JSON.parse(localStorage.getItem(projectStorageKey));
      if (Array.isArray(saved)) return new Map(saved.filter(item => typeof item?.id === "string"
        && typeof item.title === "string" && Array.isArray(item.folders)).map(item => [item.id, item]));
    } catch {
      // The current session still works without storage.
    }
    return new Map();
  }

  function saveProjectRegistrations() {
    try {
      const saved = [...registrations.values()].map(entry => ({ ...entry, folders: entry.folders.map(folder =>
        ({ id: folder.id, name: folder.name, path: folder.path })) }));
      localStorage.setItem(projectStorageKey, JSON.stringify(saved));
    } catch {
      toast("浏览器空间不足，项目变更仅保存在本次会话。");
    }
  }

  function projectGroups() {
    return [...new Set([workspace, ...projects().map(item => item.id), ...registrations.keys(),
      ...threads.filter(thread => thread.mode === "edit" && (thread.messages.length || thread.archived)).map(thread => thread.project)])]
      .filter(id => id && !registrations.get(id)?.removed);
  }

  function projectWorkspaceVideos(id) {
    return workspaceFolders(id).flatMap(folder => (folder.files || []).map(entry => lookupAsset(entry.assetId)).filter(Boolean));
  }

  function workspaceFolders(id) {
    const chosen = projectWorkspace(id);
    if (projectWorkspaceReplaced(id)) return chosen;
    const savedProject = editorProject(projectEditorId(id));
    const mediaPath = savedProject?.path || lookupAsset(savedProject?.assetId)?.path;
    const root = id === workspace ? state.settings.outputPath : mediaPath?.replace(/[^\\/]+$/, "");
    if (!root || !/^(?:[a-z]:[\\/]|[\\/]{2}|\/)/i.test(root)) return chosen;
    const key = path => path.replace(/\\/g, "/").replace(/\/$/, "").toLowerCase();
    if (chosen.some(folder => key(folder.path) === key(root))) return chosen;
    let inferred = inferredWorkspaceFolders.get(id);
    if (!inferred || inferred.path !== root) {
      inferred = { id: crypto.randomUUID(), path: root, name: key(root).split("/").pop() || root };
      inferredWorkspaceFolders.set(id, inferred);
    }
    return [inferred, ...chosen];
  }

  function load() {
    try {
      const saved = JSON.parse(localStorage.getItem(storageKey));
      if (Array.isArray(saved)) return saved.filter(thread => typeof thread?.id === "string"
        && ["record", "edit"].includes(thread.mode) && Array.isArray(thread.messages)).map(thread => ({
          ...thread, pending: null, params: { ...defaultParams, ...thread.params },
          materials: Array.isArray(thread.materials) ? thread.materials : [],
        }));
    } catch {
      // The current session still works without storage.
    }
    return [];
  }

  function persist() {
    try {
      const entries = threads.filter(thread => thread.messages.length || thread.materials.length || thread.archived);
      localStorage.setItem(storageKey, JSON.stringify(entries.map(({ pending, ...thread }) => thread)));
    } catch {
      // Keep the full in-memory history even if browser storage is full.
    }
  }

  function touched(thread) {
    const last = threads.reduce((latest, item) => Math.max(latest, item.visited || 0), 0);
    thread.visited = Math.max(Date.now(), last + 1);
    thread.unread = false;
  }

  function ordered(kind, includeArchived = false) {
    return threads.filter(thread => (!kind || thread.mode === kind) && thread.archived === includeArchived
      && (thread.messages.length || thread.materials.length || thread.archived)).sort((a, b) => b.visited - a.visited);
  }

  function material(asset, from = 0, to = asset.duration, name = "") {
    return { id: crypto.randomUUID(), asset: asset.id, from, to, name, note: "", marked: false, selected: true };
  }

  function newThread(kind, projectId, origin = "workbench") {
    const asset = kind === "edit" ? lookupAsset(asAssetId(projectId)) : null;
    const thread = {
      id: crypto.randomUUID(), mode: kind, project: projectId, origin, visited: 0, archived: false, unread: false,
      messages: [], params: { ...defaultParams },
      materials: asset ? [material(asset)] : [],
    };
    touched(thread);
    threads.push(thread);
    if (origin === "sidebar") sideThreads.set(kind, thread.id);
    else active.set(scope(kind, projectId), thread.id);
    return thread;
  }

  function getThread(kind, projectId = "") {
    const key = scope(kind, projectId);
    const previous = byId(active.get(key));
    if (previous && !previous.archived) return previous;
    const latest = ordered(kind).find(thread => thread.origin !== "sidebar" && (kind === "record" || thread.project === projectId));
    if (!latest) return newThread(kind, projectId);
    active.set(key, latest.id);
    touched(latest);
    persist();
    return latest;
  }

  function workbenchThread() {
    if (mode === "record") return getThread("record");
    const groups = projectGroups();
    if (!groups.includes(project)) project = groups.includes(currentProject()) ? currentProject() : groups[0] || "";
    return project ? getThread("edit", project) : null;
  }

  function isVisible(thread) {
    if (state.closed || state.minimized || document.visibilityState === "hidden") return false;
    if (state.page === "ai") return workbenchThread()?.id === thread.id;
    return Boolean(assistantPane()?.open && sideThreads.get(sidebarMode()) === thread.id);
  }

  function sidebarMode() {
    return state.page === "editor" ? "edit" : "record";
  }

  function clearSidebarThread(kind) {
    const previous = byId(sideThreads.get(kind));
    sideThreads.delete(kind);
    if (kind === "edit" && previous?.pending) cancelThreadJob(previous, false);
    if (previous && !previous.messages.length && !previous.pending && !previous.archived) {
      drafts.delete(previous.id);
      threads = threads.filter(thread => thread !== previous);
    }
  }

  function sidebarThread() {
    const kind = sidebarMode();
    if (kind === "edit" && !hasExplicitEditorProject()) return null;
    const projectId = kind === "edit" ? currentProject() : "";
    const previous = byId(sideThreads.get(kind));
    if (previous && !previous.archived && previous.project === projectId) return previous;
    clearSidebarThread(kind);
    return newThread(kind, projectId, "sidebar");
  }

  function chooseThread(id) {
    const thread = byId(id);
    if (!thread || thread.archived) return;
    mode = thread.mode;
    if (mode === "edit") {
      project = thread.project;
      closedProjectGroups.delete(project);
    }
    active.set(scope(mode, thread.project), id);
    touched(thread);
    persist();
    refresh();
  }

  function spinner(label = "处理中") {
    return `<i class="agent-spinner" role="status" aria-label="${label}"></i>`;
  }

  function title(thread) {
    return thread.messages.find(message => message.who === "user")?.text.replace(/\s+/g, " ").slice(0, 50) || "新对话";
  }

  function historyRow(thread, selected) {
    const name = title(thread);
    return `<div class="agent-thread-row${selected ? " active" : ""}">
      <button class="agent-thread-name" type="button" data-action="aiw-thread" data-value="${esc(thread.id)}"
        title="${esc(name)}" aria-current="${selected ? "true" : "false"}"><span class="truncate">${esc(name)}</span>
        ${thread.pending ? spinner() : thread.unread ? '<i class="agent-unread" aria-label="未读" role="img"></i>' : ""}</button>
      ${ib("aiw-archive", "归档对话", "folder", { value: thread.id })}</div>`;
  }

  function history(selected) {
    const entries = ordered(mode);
    if (mode === "record") return entries.length
      ? entries.map(thread => historyRow(thread, thread.id === selected)).join("")
      : '<small class="agent-empty">暂无对话</small>';
    const groups = Map.groupBy(entries, thread => thread.project);
    const current = byId(selected);
    if (current && !current.messages.length && !current.archived && (current.started || !registrations.get(current.project)?.created)) {
      groups.set(current.project, [current, ...(groups.get(current.project) || [])]);
    }
    const ids = projectGroups()
      .sort((a, b) => (groups.get(b)?.[0]?.visited || 0) - (groups.get(a)?.[0]?.visited || 0));
    return ids.map(id => {
      const records = groups.get(id) || [];
      const expanded = !closedProjectGroups.has(id);
      const name = projectTitle(id);
      return `<section class="agent-history-group" data-project-group="${esc(id)}">
        <div class="agent-project-row${id === project ? " active" : ""}">
          <button type="button" class="btn flat mini icon-btn agent-folder-toggle" data-action="aiw-folder-toggle"
            data-value="${esc(id)}" aria-expanded="${expanded}" aria-label="${expanded ? "收起" : "展开"} ${esc(name)}">${g("right")}</button>
          <button class="agent-project-name" type="button" data-action="aiw-project-select" data-value="${esc(id)}"
            aria-pressed="${id === project}" title="${esc(name)}">${g("folder")}<span class="truncate">${esc(name)}</span></button>
          ${expanded ? "" : records.some(thread => thread.pending) ? spinner()
            : records.some(thread => thread.unread) ? '<i class="agent-unread" role="img" aria-label="未读"></i>' : ""}
          <div class="agent-project-tools">
            ${ib("aiw-project-new-thread", "新建对话", "plus", { value: id })}
            <button type="button" class="btn flat mini icon-btn agent-project-more" data-action="aiw-project-settings"
              data-value="${esc(id)}" aria-label="项目设置" aria-haspopup="dialog" title="项目设置">⋯</button>
          </div>
        </div>
        <div class="agent-project-threads"${expanded ? "" : " hidden"}>${records.map(thread =>
          historyRow(thread, thread.id === selected)).join("")}</div>
      </section>`;
    }).join("");
  }

  function registerProject(name, details) {
    const id = details.editorId || "assistant-" + crypto.randomUUID();
    if (details.editorId) {
      const existing = registrations.get(id);
      registrations.set(id, { id, title: name, folders: existing?.folders ?? [], ...existing, ...details, removed: false });
    } else {
      const titles = new Set(projectGroups().map(projectTitle));
      let title = name;
      for (let number = 2; titles.has(title); number++) title = `${name} (${number})`;
      registrations.set(id, { id, title, folders: [], created: true, ...details });
    }
    saveProjectRegistrations();
    project = id;
    closedProjectGroups.delete(id);
    dialog.close();
    refresh();
  }

  async function pickWorkspaceFolder(operation, id) {
    try {
      const selection = await Native.rpc(operation === "project" ? "workspace-project" : "workspace-folder");
      if (!selection?.id) return;
      const folder = workspaceFolder(selection);
      workspaceLoads.set(folder.id, { done: true });
      if (operation === "create" || operation === "project") {
        registerProject(selection.name, { nativeId: selection.id, project: selection.project, folders: [folder] });
        return;
      }
      const registration = registrations.get(id) || { id, title: projectTitle(id), folders: [] };
      const prior = registration.folders.find(item => item.path.toLowerCase() === folder.path.toLowerCase());
      if (prior && operation === "add") {
        workspaceLoads.delete(folder.id);
        toast("这个文件夹已加入工作区。");
        return;
      }
      if (operation === "replace") registration.folders.forEach(item => workspaceLoads.delete(item.id));
      registration.folders = operation === "replace" ? [folder] : [...registration.folders, folder];
      registration.replaced = operation === "replace" || registration.replaced;
      registrations.set(id, registration);
      saveProjectRegistrations();
      dialog.close();
      refresh();
    } catch (error) { reportNativeError(error); }
  }

  function workspaceFolder(selection) {
    return { id: selection.id, path: selection.folder, name: selection.root_name || selection.name, truncated: selection.truncated,
      files: selection.files.map(({ asset, ...file }) => {
        if (asset?.id) workspaceAssets.set(asset.id, { ...lookupAsset(asset.id), ...asset,
          duration: lookupAsset(asset.id)?.duration || asset.duration, thumbnails: lookupAsset(asset.id)?.thumbnails || [] });
        return { ...file, assetId: asset?.id };
      }) };
  }

  function refreshWorkspace(id, force = false) {
    const registration = registrations.get(id);
    return Promise.all(workspaceFolders(id).map(folder => {
      const current = workspaceLoads.get(folder.id);
      if (current?.promise) return current.promise;
      if (current?.done && !force) return;
      const status = {};
      workspaceLoads.set(folder.id, status);
      status.promise = Native.rpc("workspace-refresh", { workspace: folder.id, folder: folder.path,
        project: registration?.nativeId === folder.id ? registration.project : "" }).then(selection => {
        if (workspaceFolders(id).includes(folder)) Object.assign(folder, workspaceFolder(selection), { error: "" });
      }).catch(error => {
        folder.error = error.message;
        folder.files = [];
      }).finally(() => {
        status.done = true;
        delete status.promise;
        if (state.page === "ai" && project === id) refresh();
      });
      return status.promise;
    }));
  }

  async function assureDuration(asset) {
    if (asset?.duration > 0) return asset;
    if (!asset?.id) throw new Error("请先选择视频。");
    if (workspaceMediaLoads.has(asset.id)) return workspaceMediaLoads.get(asset.id);
    const task = Native.rpc("workspace-media", { id: asset.id }).then(info => {
      if (!(info?.duration > 0)) throw new Error("无法读取该视频的时间信息。");
      return Object.assign(asset, info);
    }).finally(() => workspaceMediaLoads.delete(asset.id));
    workspaceMediaLoads.set(asset.id, task);
    return task;
  }

  function createProjectDialog() {
    const entries = projects().map(item => `<label class="project-picker-row">
      <input type="radio" name="editor" value="${esc(item.id)}" required>
      <span class="truncate">${esc(item.title)}</span></label>`).join("");
    showDialog("agent-project-create", "新建项目", `<form class="stack" id="agent-project-picker" aria-label="选择剪辑工程">${entries}</form>
      `, b("aiw-project-folder", "选择文件夹…", "folder", { variant: "mini" }) + b("aiw-project-file", "选择工程文件…", "folder", { variant: "mini" }) +
      `<button class="btn primary" type="submit" form="agent-project-picker"${entries ? "" : " disabled"}>使用选中工程</button>`);
    dialog.querySelector("#agent-project-picker").addEventListener("submit", event => {
      event.preventDefault();
      const linked = editorProject(new FormData(event.currentTarget).get("editor"));
      if (linked) registerProject(linked.title, { editorId: linked.id });
    });
  }

  function projectSettingsDialog(id) {
    if (!projectGroups().includes(id)) return;
    const names = workspaceFolders(id).map(folder => `<li title="${esc(folder.path)}">${esc(folder.name)}</li>`).join("");
    const actions = (action, label, icon, variant = "") => b(action, label, icon, { value: id, variant });
    showDialog("agent-project-settings", projectTitle(id), `
      ${names ? `<ul class="agent-workspace-roots">${names}</ul>` : ""}
      <div class="agent-project-options">
        ${actions("aiw-project-rename", "重命名", "pen")}
        ${actions("aiw-project-change-folder", "更改工作区", "folder")}
        ${actions("aiw-project-add-folder", "添加工作区文件夹", "plus")}
        ${actions("aiw-project-remove", "删除项目", "trash", "danger")}
      </div>`);
  }

  function renameProjectDialog(id) {
    if (!projectGroups().includes(id)) return;
    showDialog("agent-project-rename", "重命名项目", `<form class="form" id="agent-project-rename">
      <label for="agent-project-name">项目名称</label><input type="text" id="agent-project-name"
        name="title" maxlength="120" required value="${esc(projectTitle(id))}"></form>`,
      '<button type="submit" class="btn primary" form="agent-project-rename">保存</button>');
    const form = dialog.querySelector("form");
    const input = form.elements.title;
    input.focus();
    input.select();
    input.addEventListener("input", () => input.setCustomValidity(""));
    form.addEventListener("submit", event => {
      event.preventDefault();
      const title = input.value.trim();
      if (!title) {
        input.setCustomValidity("请输入项目名称。");
        input.reportValidity();
        return;
      }
      const registration = registrations.get(id) || { id, folders: [] };
      registrations.set(id, { ...registration, title });
      saveProjectRegistrations();
      dialog.close();
      refresh();
    });
  }

  function confirmProjectRemoval(id) {
    if (!projectGroups().includes(id)) return;
    showDialog("agent-project-remove", "删除项目", `<p>删除「${esc(projectTitle(id))}」及其剪辑助手对话？工程和工作区文件会保留。</p>`,
      b("aiw-project-confirm-remove", "删除项目", "trash", { value: id, variant: "danger" }));
  }

  function removeProject(id) {
    if (!projectGroups().includes(id)) return;
    const record = registrations.get(id);
    for (const folder of workspaceFolders(id)) workspaceLoads.delete(folder.id);
    inferredWorkspaceFolders.delete(id);
    if (record?.created) registrations.delete(id);
    else registrations.set(id, { id, title: projectTitle(id), folders: [], removed: true });
    saveProjectRegistrations();
    const removed = threads.filter(thread => thread.mode === "edit" && thread.project === id && thread.origin !== "sidebar");
    for (const thread of removed) {
      clearTimeout(jobs.get(thread.id));
      jobs.delete(thread.id);
      drafts.delete(thread.id);
    }
    threads = threads.filter(thread => !removed.includes(thread));
    active.delete(scope("edit", id));
    closedProjectGroups.delete(id);
    rightLayouts.delete(id);
    persist();
    dialog.close();
    refresh();
  }

  function paramsPanel(thread, side) {
    const params = thread.params;
    const ranges = side ? [["full", "工程全片"], ["selected", "选中视频片段"], ["custom", "时间轴区间"]]
      : [["full", "所选素材全片"], ["selected", "所选素材的选段"], ["custom", "各素材指定区间"]];
    return `<div class="agent-params" data-agent-thread="${esc(thread.id)}">
      <div class="agent-param-grid">
        <label for="agent-range-${side ? "side" : "main"}">${tr("分析范围")}</label>
        ${select("aiw-range", "分析范围", ranges, params.range, `id="agent-range-${side ? "side" : "main"}"
          title="${side ? "工程全片分析时间轴中的全部视频。选中视频片段只分析已选中的那一段。" : "全片分析勾选素材的完整原视频。选段使用素材页填写的起止时间。"}"`)}
        ${params.range === "custom" ? `<label>${tr("起止时间")}</label><div class="agent-time-fields">
          <input type="number" min="0" step="0.1" aria-label="开始秒数" data-field="aiw-from" value="${params.from}">
          <input type="number" min="0.1" step="0.1" aria-label="结束秒数" data-field="aiw-to" value="${params.to}"><small>${tr("秒")}</small></div>` : ""}
        <label for="agent-target-${side ? "side" : "main"}">${tr("目标时长")}</label>
        <div class="hstack"><input type="range" id="agent-target-${side ? "side" : "main"}" class="grow"
          min="5" max="180" step="5" data-agent-range value="${params.target}">
          <output data-agent-target class="mono">${params.target} ${tr("秒")}</output></div>
      </div>
      <div class="hstack agent-cues">${[["subtitles", "字幕"], ["markers", "打点"], ["picture", "画面"]].map(([id, label]) =>
        `<label class="hstack"><input type="checkbox" data-field="aiw-${id}"${params[id] ? " checked" : ""}>${tr(label)}</label>`).join("")}</div>
    </div>`;
  }

  function messageTargets(message, index) {
    if (!message.targets?.length) return "";
    const montage = message.smartCut?.montage;
    return `<div class="agent-result-list">${message.targets.map((target, row) => `<div class="hstack">
      ${montage ? `<input type="checkbox" aria-label="选择录像 ${esc(target.label)}" data-agent-montage="${index}:${row}"${target.selected ? " checked" : ""}>` : ""}
      ${b(target.action === "edit" ? "aiw-locate" : "aiw-preview", target.label, target.action === "edit" ? "flag" : "play",
        { variant: "mini", value: JSON.stringify([target.id, target.at]), optional: true })}
      ${message.smartCut && !montage && !message.cut?.targets.length ? ib("aiw-search-cut", "分析该录像", "scissors", { value: `${index}:${row}` }) : ""}</div>`).join("")}
      ${montage ? b("aiw-search-cut", "分析勾选录像", "scissors", { variant: "mini", value: `${index}:all` }) : ""}</div>`;
  }

  function settingsChanges(preview) {
    const labels = { resolution: "分辨率", fps: "帧率", bitrate_kbps: "码率（kbps）", audio: "音频来源",
      encoder: "编码器", format: "格式", separate_audio: "独立音轨", denoise: "降噪", cursor: "录制鼠标" };
    const format = value => typeof value === "boolean" ? value ? "开" : "关" :
      ({ auto: "自动", none: "无", mic: "麦克风", system: "系统声音", mix: "麦克风和系统声音" })[value] || value;
    return `<dl class="agent-diff">${preview.diff.map(item => `<dt>${esc(labels[item.field] || item.field)}</dt>
      <dd>${esc(format(item.before))} → ${esc(format(item.after))}</dd>`).join("")}</dl>`;
  }

  function candidateDraft(message, messageIndex) {
    if (!message.cut?.targets.length) return "";
    const cut = message.cut;
    const selected = cut.targets.filter(item => item.selected !== false);
    const duration = selected.reduce((total, item) => total + item.end - item.start, 0);
    const multiple = new Set(selected.map(item => item.id)).size > 1;
    const reorder = new Set(cut.targets.map(item => item.id)).size > 1;
    return `<div class="agent-cut"><div class="agent-cut-summary">候选片段 <small class="mono">${selected.length} 段 · ${duration.toFixed(2)} 秒</small></div>
      ${cut.targets.map((item, index) => `<article class="agent-cut-row">
        <label><input type="checkbox" data-agent-candidate="${messageIndex}:${index}" aria-label="选用 ${esc(item.label)}"${item.selected === false ? "" : " checked"}>
        ${esc(item.label || "片段")}</label><small>${esc(lookupAsset(item.id)?.title)}${item.reason ? " · " + esc(item.reason) : ""}</small>
        <div class="hstack"><input type="number" step=".001" min="${item.scope_start}" max="${item.scope_end}" value="${item.start}" aria-label="起点（秒）"
          data-agent-candidate="${messageIndex}:${index}" data-edge="start"> <span>–</span>
          <input type="number" step=".001" min="${item.scope_start}" max="${item.scope_end}" value="${item.end}" aria-label="终点（秒）"
          data-agent-candidate="${messageIndex}:${index}" data-edge="end">
        ${ib("aiw-cut-preview", "预览片段", "play", { value: `${messageIndex}:${index}` })}
        ${reorder ? ib("aiw-cut-order", "上移", "up", { value: `${messageIndex}:${index}:-1`, disabled: index === 0 }) +
          ib("aiw-cut-order", "下移", "down", { value: `${messageIndex}:${index}:1`, disabled: index === cut.targets.length - 1 }) : ""}</div></article>`).join("")}
      ${cut.applied ? "<small>已应用到剪辑工坊</small>" : `<div class="hstack">
        ${b("aiw-cut-apply", "预览并应用", "check", { variant: "mini", value: messageIndex, disabled: !selected.length || multiple })}
        ${b("aiw-cut-export", "保存多素材草稿", "save", { variant: "mini", value: messageIndex, disabled: !multiple })}</div>`}</div>`;
  }

  function composer(thread, side = false) {
    const greeting = { who: "assistant", text: UI.text(thread.mode === "record"
      ? "我是 WPS Recorder 录屏助手。可以按名称、打点或字幕查找录像，排查录制问题，或预览录制参数。"
      : side ? "以「%1」为当前工程，可以按时间轴片段和打点梳理素材。"
      : "可以按素材、打点和片段整理剪辑草稿。").replace("%1", () => projectTitle(thread.project)),
      options: starters[thread.mode] };
    const messages = [greeting, ...thread.messages];
    const canCompact = ConversationCompact.eligible(thread);
    return `<div class="chat-log agent-log" data-chat-log="agent" role="log" aria-live="polite">
      ${messages.map((message, index) => `<div class="message ${message.who === "user" ? "user" : "assistant"}" data-agent-thread="${esc(thread.id)}"><p>${esc(message.text)}</p>${messageTargets(message, index - 1)}
        ${candidateDraft(message, index - 1)}${message.settings?.diff?.length ? settingsChanges(message.settings) +
        b("aiw-apply-settings", "确认应用参数", "check", { variant: "mini", value: JSON.stringify(message.settings) }) : ""}
        ${!thread.pending && index === messages.length - 1 && message.options?.length ? `<div class="message-choices">
          ${message.options.map(label => b("aiw-prompt", label, null, { value: label, variant: "mini" })).join("")}</div>` : ""}</div>`).join("")}
      ${thread.pending ? `<div class="message agent-pending" data-agent-thread="${esc(thread.id)}">${spinner("生成回复")}<span>${tr("生成回复")}</span>
        ${b("aiw-cancel", "停止", null, { variant: "flat mini" })}</div>` : ""}
    </div><div class="agent-input-area" data-agent-thread="${esc(thread.id)}" data-agent-side="${side}">
      ${ConversationCompact.status(thread)}
      ${thread.mode === "edit" ? paramsPanel(thread, side) : ""}
      <form class="chat-compose" data-agent-form="compose">
        <textarea name="prompt" rows="1" maxlength="12000" placeholder="${tr(thread.mode === "record" ? "查找录像、排查故障或调整参数…" : "告诉助手怎样剪辑…")}"
          aria-label="${tr("输入问题")}" required>${esc(drafts.get(thread.id))}</textarea>
        <div class="chat-compose-actions">
          <span${canCompact ? "" : ` title="${tr("压缩上下文")}"`}>
            ${ib("aiw-compact", "压缩上下文", "layers", { variant: "flat", disabled: !canCompact })}
          </span>
          <button type="submit" class="btn primary icon-btn" aria-label="${tr("发送")}" title="${tr("发送")}"${thread.pending ? " disabled" : ""}>${g("send")}</button>
        </div>
      </form></div>`;
  }

  function assetPreview(asset) {
    return asset?.poster ? `<img src="${esc(asset.poster)}" alt="">` : `<span class="agent-file-icon">${g("film")}</span>`;
  }

  function materialRow(item) {
    const asset = lookupAsset(item.asset);
    const isSlice = item.name || item.from > 0 || item.to < asset?.duration;
    const name = item.name || (isSlice ? asset?.title + " · 片段" : asset?.title) || "未载入视频";
    return `<article class="agent-material">
      <input type="checkbox" data-field="aiw-selected" data-id="${esc(item.id)}" aria-label="选用 ${esc(name)}"${item.selected ? " checked" : ""}>
      ${assetPreview(asset)}<div class="agent-material-info">
        <strong class="truncate" title="${esc(name)}">${esc(name)}</strong>
        <small class="mono">${isSlice ? `${time(item.from)} – ${time(item.to)}` : time(item.to)}</small>
        ${item.note ? `<small class="truncate" title="${esc(item.note)}">${esc(item.note)}</small>` : ""}
      </div><div class="agent-row-tools">
        ${ib("aiw-mark", item.marked ? "取消标记" : "标记重点", "flag", { value: item.id, pressed: item.marked })}
        ${ib("aiw-configure", "素材设置", "cog", { value: item.id, disabled: !asset })}
        ${ib("aiw-remove", "从本对话移除", "minus", { value: item.id })}
      </div></article>`;
  }

  function workspaceContent(thread) {
    const editorId = projectEditorId(thread.project);
    const entry = projects().find(item => item.id === editorId);
    const source = lookupAsset(entry?.assetId);
    const localRoots = workspaceFolders(thread.project);
    const localVideos = projectWorkspaceVideos(thread.project);
    const videos = thread.project === workspace ? assets : source ? [source] : [];
    return { roots: localRoots.map(folder => ({ ...folder, loading: !!workspaceLoads.get(folder.id)?.promise })),
      videos: [...new Map([...videos, ...localVideos].map(asset => [asset.id, asset])).values()],
      workspace: localRoots[0]?.id, folder: localRoots[0]?.path };
  }

  function rightLayout() {
    let layout = rightLayouts.get(project);
    if (!layout) {
      const tab = { id: crypto.randomUUID(), page: "" };
      layout = { tabs: [tab], selected: tab.id };
      rightLayouts.set(project, layout);
    }
    return layout;
  }

  function resetWorkbenchLayout() {
    closedProjectGroups.clear();
    rightLayouts.clear();
  }

  function selectRightTab(id) {
    const layout = rightLayout();
    const tab = layout.tabs.find(tab => tab.id === id);
    if (!tab) return;
    layout.selected = id;
    if (tab.page === "workspace") refreshWorkspace(project, true);
    refresh();
    const button = document.getElementById("agent-tab-" + id);
    button?.focus({ preventScroll: true });
    button?.scrollIntoView({ block: "nearest", inline: "nearest" });
  }

  function createRightTab() {
    const tab = { id: crypto.randomUUID(), page: "" };
    rightLayout().tabs.push(tab);
    selectRightTab(tab.id);
  }

  function closeRightTab(id) {
    const layout = rightLayout();
    const index = layout.tabs.findIndex(tab => tab.id === id);
    if (index < 0) return;
    layout.tabs.splice(index, 1);
    if (!layout.tabs.length) { createRightTab(); return; }
    selectRightTab(layout.selected === id ? layout.tabs[Math.min(index, layout.tabs.length - 1)].id : layout.selected);
  }

  function materialPage(thread) {
    const openId = thread.project === workspace
      ? projects().find(entry => entry.assetId === thread.materials.find(item => item.selected)?.asset)?.id : projectEditorId(thread.project);
    return `${head("素材与片段", "film", ib("aiw-add", "添加素材或片段", "plus"))}
      <div class="scroll-body agent-material-list">
        ${thread.materials.map(materialRow).join("") || '<small class="agent-empty">暂无素材</small>'}
      </div><div class="panel-foot agent-material-foot">
        ${b("aiw-open-editor", "进入剪辑工坊", "scissors", { value: openId, variant: "mini", disabled: !openId })}
      </div>`;
  }

  function materialsPanel(thread) {
    const layout = rightLayout();
    const current = layout.tabs.find(tab => tab.id === layout.selected);
    const tabs = layout.tabs.map(tab => {
      const [label, icon] = rightPages[tab.page] || ["空白页", "window"];
      const selected = tab.id === current.id;
      return `<div class="agent-right-tab${selected ? " active" : ""}">
        <button type="button" class="agent-tab-button" role="tab" data-action="aiw-tab-select"
          data-value="${tab.id}" id="agent-tab-${tab.id}" aria-controls="agent-tab-panel"
          aria-selected="${selected}" tabindex="${selected ? "0" : "-1"}">${g(icon)}<span>${tr(label)}</span></button>
        ${ib("aiw-tab-close", "关闭" + label, "close", { value: tab.id })}</div>`;
    }).join("");
    const body = current.page === "materials" ? materialPage(thread) : current.page
      ? AgentWorkspacePages.render(current.page, current, workspaceContent(thread))
      : `<div class="agent-right-chooser">${Object.entries(rightPages).map(([value, [label, icon]]) =>
        b("aiw-tab-page", label, icon, { value, variant: "mini" })).join("")}</div>`;
    return `<section class="panel ai-candidates agent-material-panel" data-agent-thread="${esc(thread.id)}">
      <div class="panel-head agent-right-head"><div class="agent-right-tabs" role="tablist" aria-label="右侧页面">${tabs}</div>
        ${ib("aiw-tab-new", "新建空白页", "plus")}</div>
      <div class="agent-right-content" id="agent-tab-panel" role="tabpanel" aria-labelledby="agent-tab-${current.id}">${body}</div></section>`;
  }

  function page() {
    const thread = workbenchThread();
    if (thread?.mode === "edit") refreshWorkspace(thread.project);
    if (thread?.unread) { thread.unread = false; persist(); }
    const editing = mode === "edit";
    return `<div class="page ai-page agent-workbench${editing ? " agent-edit" : " agent-record"}${state.ai.collapsed ? " ai-slim" : ""}${editing && state.ai.hide ? " ai-no-" + state.ai.hide : ""}"
      style="--ai-right:${state.width.ai}%">
      <aside class="panel ai-nav agent-nav">
        ${head("AI 助手", "spark", ib("ai-collapse", "收起／展开功能选择", "collapse"))}
        <div class="agent-mode-list ai-nav-content">${Object.entries(modeTitles).map(([id, label]) =>
          b("aiw-mode", label, id === "record" ? "bot" : "scissors", { variant: "flat", value: id, pressed: mode === id })).join("")}</div>
        <div class="agent-history-title ai-nav-content"><small>${tr("历史对话")}</small>
          ${editing ? ib("aiw-project-create", "新建项目", "plus") : ib("aiw-new", "新对话", "plus")}</div>
        <div class="agent-history ai-nav-content">${history(thread?.id)}</div>
        <div class="panel-foot agent-nav-foot ai-nav-content">${b("settings", "模型服务", "chip", { variant: "flat mini", value: "models" })}</div>
      </aside>
      <section class="panel ai-middle agent-center"${thread ? ` data-agent-thread="${esc(thread.id)}"` : ""}>
        ${head(modeTitles[mode], editing ? "scissors" : "bot")}${thread ? composer(thread) : `<div class="agent-right-chooser">
          ${b("aiw-project-create", "新建项目", "plus", { variant: "mini" })}</div>`}
      </section>
      ${editing && thread ? `<div class="splitter" data-divider="ai" role="separator" tabindex="0" aria-orientation="vertical" aria-label="调整对话和素材区域宽度"></div>
        ${materialsPanel(thread)}` : ""}
    </div>`;
  }

  function sidebar() {
    const kind = sidebarMode();
    const icon = kind === "edit" ? "scissors" : "bot";
    const close = ib("sidebar-ai", "关闭侧栏", "close");
    const thread = sidebarThread();
    if (!thread) {
      return head(modeTitles[kind], icon, close) + `<div class="agent-project-gate inset stack">
        <p class="soft">先选择剪辑工程。</p>${b("open-editor-project", "选择工程", "folder", { variant: "primary" })}</div>`;
    }
    if (thread.unread) { thread.unread = false; persist(); }
    return head(modeTitles[kind], icon, `<div class="hstack">
      ${ib("aiw-new", "新对话", "plus")}${close}</div>`) + composer(thread, true);
  }

  function archivedSettings() {
    const rows = ordered(null, true);
    return `<div class="agent-archive"><h3>已归档对话</h3>${rows.length ? rows.map(thread => `<div class="agent-archive-row">
      <div class="agent-material-info"><strong class="truncate" title="${esc(title(thread))}">${esc(title(thread))}</strong>
        <small>${esc(thread.mode === "edit" ? "剪辑助手 · " + projectTitle(thread.project) : "录屏助手")}</small></div>
      ${b("aiw-restore", "还原", "restore", { value: thread.id, variant: "mini" })}
      ${ib("aiw-delete", "删除对话", "trash", { value: thread.id })}</div>`).join("") : '<small class="agent-empty">暂无归档对话</small>'}</div>`;
  }

  function refresh() {
    const focused = document.activeElement;
    const compose = focused?.closest("[data-agent-form='compose']");
    const id = compose?.closest("[data-agent-thread]")?.dataset.agentThread;
    const selection = focused?.matches("textarea") ? [focused.selectionStart, focused.selectionEnd] : null;
    const searchTab = focused?.matches(".agent-workspace-search input") ? focused.closest("[data-agent-workspace-tab]")?.dataset.agentWorkspaceTab : "";
    const treeScroll = searchTab ? focused.closest("[data-agent-workspace-tab]").querySelector(".agent-workspace-tree")?.scrollTop : 0;
    if (state.page === "ai" && !state.closed && !state.minimized) render();
    else if (assistantPane()?.open && !state.closed && !state.minimized) drawAssistant(true);
    if (selection && id) {
      const next = [...document.querySelectorAll("[data-agent-form='compose'] textarea")].find(input =>
        input.closest("[data-agent-thread]")?.dataset.agentThread === id);
      next?.focus({ preventScroll: true });
      next?.setSelectionRange(...selection);
    }
    if (searchTab) {
      const panel = document.querySelector(`[data-agent-workspace-tab="${CSS.escape(searchTab)}"]`);
      panel?.querySelector(".agent-workspace-search input")?.focus({ preventScroll: true });
      if (panel && treeScroll) panel.querySelector(".agent-workspace-tree").scrollTop = treeScroll;
    }
  }

  function resetSession() {
    mode = "record";
    project = "";
    resetWorkbenchLayout();
    state.ai.hide = "";
    state.ai.collapsed = false;
    active.clear();
    clearSidebarThread("record");
    clearSidebarThread("edit");
    resetAssistantPanes();
    drafts.clear();
    for (const id of jobs.keys()) cancelThreadJob(byId(id));
    ConversationCompact.reset();
  }

  document.addEventListener("editorprojectchange", () => {
    clearSidebarThread("edit");
    if (state.page === "editor" && assistantPane()?.open && !state.closed && !state.minimized) drawAssistant(true);
  });

  window.addEventListener("focus", () => {
    if (state.page !== "ai" || mode !== "edit" || state.closed || state.minimized) return;
    const layout = rightLayouts.get(project);
    if (layout?.tabs.find(tab => tab.id === layout.selected)?.page === "workspace") refreshWorkspace(project, true);
  });

  function preview(id, at = 0) {
    const asset = lookupAsset(id);
    if (!asset) { toast("请先载入该视频。"); return; }
    reviewMedia(asset.id, at);
  }

  function locate(id, at) {
    if (currentProject() === id && state.page === "editor") seekPlayer("editor", at);
    else openEditorProject(id, at);
  }

  function cancelThreadJob(thread, redraw = true) {
    if (!thread?.pending) return;
    const job = jobs.get(thread.id);
    if (job?.native) Native.tell("assistant-cancel", { job: job.native });
    jobs.delete(thread.id);
    thread.pending = null;
    persist();
    if (redraw) refresh();
  }

  async function scheduleReply(thread) {
    if (!thread.pending || jobs.has(thread.id)) return;
    const token = thread.pending;
    const job = {};
    jobs.set(thread.id, job);
    const valid = () => jobs.get(thread.id) === job && thread.pending === token && byId(thread.id) === thread;
    const rpc = (command, data) => {
      if (!valid()) throw new Error("任务已取消。");
      job.native = crypto.randomUUID();
      return Native.rpc(command, { ...data, job: job.native });
    };
    if (token.cutRequest) {
      try {
        const cut = await rpc("suggest-cut", token.cutRequest);
        finish({ text: cut.text, cut });
      } catch (error) { finish({ text: error.message }); }
      return;
    }
    const context = ConversationCompact.context(thread);
    const history = context.recent.slice(-13, -1).map(message => ({ role: message.who === "user" ? "user" : "assistant", content: message.text }));
    const params = token.params;
    const materials = token.materials;
    const editing = thread.mode === "edit";
    const projectPath = registrations.get(thread.project)?.project || "";
    const inputs = [...new Set(token.side ? [token.asset] : materials.map(item => item.asset))].filter(Boolean);
    const request = { prompt: token.prompt, history, summary: context.summary, params,
      inputs, materials: token.side ? [] : materials, projectPath, side: Boolean(token.side && editing), editing,
      selection: token.selection };
    try {
      if (editing && token.side) await flushEditorChanges();
      else if (editing) await refreshWorkspace(thread.project);
      const answer = await rpc("assistant", request);
      if (!valid()) return;
      const targets = [];
      for (const row of answer.recordings || []) {
        const asset = registerAsset(row.asset);
        if (asset) targets.push({ action: "review", id: asset.id, at: row.at_ms / 1000 || 0, label: row.excerpt || asset.title });
      }
      const automatic = answer.smart_cut?.automatic && !answer.smart_cut.montage && targets.length === 1;
      const followup = answer.edit_plan ? { ...request, plan: answer.edit_plan, revision: answer.edit_revision }
        : automatic ? foundCutRequest([targets[0].id], answer.smart_cut) : null;
      let cut, cutError;
      if (followup) {
        try { cut = await rpc("suggest-cut", followup); }
        catch (error) { cutError = error.message; }
      }
      finish({ text: [answer.text, cut?.text, cutError].filter(Boolean).join("\n"), targets, cut, settings: answer.preview,
        smartCut: answer.smart_cut, options: starters[thread.mode].filter(value => value !== token.prompt) });
    } catch (error) {
      finish({ text: error.message, options: starters[thread.mode] });
    }
    function finish(result) {
      if (!valid()) return;
      jobs.delete(thread.id);
      thread.messages.push({ who: "assistant", ...result });
      thread.pending = null;
      const visible = !thread.archived && isVisible(thread);
      thread.unread = !visible;
      if (visible) touched(thread);
      persist();
      refresh();
      maybeCompact(thread);
    }
  }

  function maybeCompact(thread, force = false) {
    if (force && !ConversationCompact.eligible(thread)) {
      toast("暂无可压缩的旧消息。");
      return;
    }
    ConversationCompact.run(thread, { force, update: () => {
      if (byId(thread.id) !== thread) return;
      persist();
      refresh();
    } });
  }

  function send(thread, prompt, side) {
    const question = String(prompt || "").trim();
    if (!question || thread.pending || thread.archived) return;
    if (question.length > 12000) { toast("一次最多输入 12000 字。"); return; }
    drafts.delete(thread.id);
    if (question.toLowerCase() === "/compact") {
      const typed = document.querySelector(`[data-agent-thread="${CSS.escape(thread.id)}"] [data-agent-form="compose"] textarea`);
      if (typed) typed.value = "";
      maybeCompact(thread, true);
      return;
    }
    const selected = thread.materials.filter(item => item.selected);
    const candidate = side && timeline.selectedTrack?.kind === "video" ? timeline.selectedClip : null;
    const clip = candidate && !candidate.stillId ? candidate : null;
    if (thread.mode === "edit" && thread.params.range === "selected" &&
        (side ? !clip : !selected.some(item => item.to > item.from))) {
      toast(side ? "请在时间轴选中一个视频片段。" : "请在素材页勾选一个有起止时间的片段。");
      return;
    }
    const query = ConversationCompact.continueRequest(thread, question);
    thread.messages.push({ who: "user", text: question });
    thread.pending = { prompt: query, side, started: Date.now(), params: { ...thread.params },
      materials: selected.map(item => ({ ...item })).sort((a, b) => Number(b.marked) - Number(a.marked)),
      asset: side && thread.mode === "edit" ? Views.editorAsset().id : "",
      selection: clip ? { start: clip.at, source: clip.source || 0, length: clip.length } : null };
    touched(thread);
    persist();
    scheduleReply(thread);
    refresh();
  }

  async function materialDialog(thread, item) {
    await refreshWorkspace(thread.project);
    const entries = workspaceContent(thread).videos;
    const asset = lookupAsset(item?.asset) || lookupAsset(asAssetId(thread.project)) || entries[0];
    if (!asset) { toast("请先在工作区添加包含视频的文件夹。"); return; }
    try { await assureDuration(asset); } catch (error) { reportNativeError(error); return; }
    if (!isVisible(thread) || byId(thread.id) !== thread) return;
    showDialog("agent-material", item ? "素材设置" : "添加素材或片段",
      `<form id="agent-material-form" class="form" data-agent-form="material"
        data-agent-thread="${esc(thread.id)}"${item ? ` data-item="${esc(item.id)}"` : ""}>
        <label for="agent-material-asset">视频</label>
        ${select("aiw-material-asset", "视频", entries.map(entry => [entry.id, entry.name]), asset.id,
          'id="agent-material-asset" name="asset"' + (item ? ' disabled' : ''))}
        <label for="agent-material-from">开始（秒）</label>
        <input id="agent-material-from" type="number" name="from" min="0" max="${asset.duration}" step="any" required value="${item?.from ?? 0}">
        <label for="agent-material-to">结束（秒）</label>
        <input id="agent-material-to" type="number" name="to" min="0" max="${asset.duration}" step="any" required value="${item?.to ?? asset.duration}">
        <label for="agent-material-name">片段名称</label>
        <input id="agent-material-name" type="text" name="name" maxlength="50" value="${esc(item?.name)}">
        <label for="agent-material-note">标注</label>
        <textarea id="agent-material-note" name="note" rows="2" maxlength="200">${esc(item?.note)}</textarea>
      </form>`, `<button class="btn primary" type="submit" form="agent-material-form">${item ? "保存" : "添加"}</button>`);
  }

  function activeSurface(control) {
    const panel = control.closest("#assistant");
    if (panel) return panel.hidden ? null : sidebarThread();
    const id = control.closest("[data-agent-thread]")?.dataset.agentThread;
    if (id) return byId(id);
    return state.page === "ai" ? workbenchThread() : null;
  }

  function candidate(thread, key) {
    const [message, index] = String(key).split(":").map(Number);
    return thread?.messages[message]?.cut?.targets[index];
  }

  function previewCandidate(row) {
    const asset = lookupAsset(row.id);
    if (!asset?.src) { toast("请刷新工作区后重新预览。"); return; }
    showDialog("agent-preview-cut", "预览片段", `<video class="agent-cut-preview" preload="metadata" controls src="${esc(asset.src)}"></video>`, "");
    const player = dialog.querySelector("video");
    player.addEventListener("loadedmetadata", () => { player.currentTime = Math.min(row.start, player.duration); }, { once: true });
    player.addEventListener("timeupdate", () => { if (player.currentTime >= row.end) player.pause(); });
    dialog.addEventListener("close", () => player.pause(), { once: true });
  }

  function confirmCut(thread, index) {
    const cut = thread?.messages[index]?.cut;
    const ranges = cut?.targets.filter(row => row.selected !== false);
    if (!ranges?.length || cut.applied || new Set(ranges.map(row => row.id)).size !== 1) return;
    showDialog("agent-confirm-cut", "应用剪辑草稿", `<p>保留已选 ${ranges.length} 段，并可在剪辑工坊撤销。</p>`,
      '<button type="button" class="btn primary" id="agent-apply-cut">确认应用</button>');
    dialog.querySelector("#agent-apply-cut").addEventListener("click", async () => {
      dialog.close();
      const first = ranges[0];
      try {
        if (first.action === "edit" && currentProject() !== thread.project) throw new Error("请打开生成草稿时的剪辑工程。");
        if (Views.editorAsset()?.id !== first.id || !hasExplicitEditorProject()) {
          if (!await openNativeProject({ id: first.id })) return;
        }
        if (timeline.tracks[0]?.locked) throw new Error("请先解锁视频轨。");
        await flushEditorChanges();
        const before = JSON.stringify(timeline.snapshot().tracks);
        const edit = await Native.rpc("apply-cut", { id: first.id, revision: first.revision,
          target: cut.target, chapters: cut.chapters, ranges });
        if (JSON.stringify(timeline.snapshot().tracks) !== before || Views.editorAsset()?.id !== first.id)
          throw new Error("剪辑工程已改变，请重新生成草稿。");
        navigate("editor");
        timeline.remember();
        timeline.tracks = edit.tracks;
        timeline.markers = edit.markers;
        timeline.selected = timeline.tracks[0]?.clips[0]?.id || "";
        timeline.track = timeline.tracks[0]?.id || "";
        timeline.setTime(0);
        timeline.onChange();
        await flushEditorChanges();
        cut.applied = true;
        persist();
        toast("已应用剪辑草稿");
      } catch (error) { reportNativeError(error); }
    }, { once: true });
  }

  function foundCutRequest(inputs, handoff) {
    return { inputs, params: { range: "full", picture: true }, plan: { ...handoff.options, chapters: handoff.chapters } };
  }

  function prepareFoundCut(thread, value) {
    const [index, result] = String(value).split(":");
    const message = thread?.messages[Number(index)];
    const options = message?.smartCut?.options;
    if (!options || thread.pending) return;
    const selected = result === "all" ? message.targets.filter(row => row.selected) : [message.targets[Number(result)]];
    const inputs = [...new Set(selected.map(row => row?.id).filter(Boolean))];
    if (inputs.length < (result === "all" ? 2 : 1)) { toast("请先选择至少两段录像。"); return; }
    thread.pending = { prompt: options.query, cutRequest: foundCutRequest(inputs, message.smartCut) };
    scheduleReply(thread);
    refresh();
  }

  const clickHandlers = {
    "aiw-mode": value => {
      if (mode === value) return;
      mode = value;
      const thread = workbenchThread();
      if (thread) touched(thread);
      persist();
      refresh();
    },
    "aiw-new": (_, thread, button) => {
      if (!thread) return;
      const side = Boolean(button.closest("#assistant"));
      if (thread.messages.length || drafts.has(thread.id)) {
        if (side) clearSidebarThread(thread.mode);
        newThread(thread.mode, thread.project, side ? "sidebar" : "workbench");
      }
      refresh();
      document.querySelector("[data-agent-form='compose'] textarea")?.focus();
    },
    "aiw-thread": chooseThread,
    "aiw-project-create": createProjectDialog,
    "aiw-project-folder": () => pickWorkspaceFolder("create"),
    "aiw-project-file": () => pickWorkspaceFolder("project"),
    "aiw-project-settings": projectSettingsDialog,
    "aiw-project-rename": renameProjectDialog,
    "aiw-project-change-folder": id => pickWorkspaceFolder("replace", id),
    "aiw-project-add-folder": id => pickWorkspaceFolder("add", id),
    "aiw-project-remove": confirmProjectRemoval,
    "aiw-project-confirm-remove": removeProject,
    "aiw-project-new-thread": id => {
      if (!projectGroups().includes(id)) return;
      const previous = byId(active.get(scope("edit", id)));
      if (previous && !previous.messages.length && !drafts.has(previous.id)) {
        threads = threads.filter(thread => thread !== previous);
      }
      project = id;
      closedProjectGroups.delete(id);
      newThread("edit", id).started = true;
      refresh();
      document.querySelector("[data-agent-form='compose'] textarea")?.focus();
    },
    "aiw-folder-toggle": id => {
      if (!closedProjectGroups.delete(id)) closedProjectGroups.add(id);
      refresh();
      const group = [...document.querySelectorAll("[data-project-group]")].find(entry => entry.dataset.projectGroup === id);
      group?.querySelector("[data-action='aiw-folder-toggle']")?.focus({ preventScroll: true });
    },
    "aiw-project-select": id => {
      project = id;
      closedProjectGroups.delete(id);
      touched(workbenchThread());
      persist();
      refresh();
      const group = [...document.querySelectorAll("[data-project-group]")].find(entry => entry.dataset.projectGroup === id);
      group?.querySelector("[data-action='aiw-project-select']")?.focus({ preventScroll: true });
    },
    "aiw-tab-new": createRightTab,
    "aiw-tab-select": selectRightTab,
    "aiw-tab-close": closeRightTab,
    "aiw-tab-page": page => {
      if (!Object.hasOwn(rightPages, page)) return;
      const layout = rightLayout();
      const tab = layout.tabs.find(item => item.id === layout.selected);
      tab.page = page;
      selectRightTab(tab.id);
    },
    "aiw-prompt": (prompt, thread, button) => { if (thread) send(thread, prompt, !!button.closest("#assistant")); },
    "aiw-cancel": (_, thread) => cancelThreadJob(thread),
    "aiw-compact": (_, thread) => { if (thread) maybeCompact(thread, true); },
    "aiw-preview": value => preview(...JSON.parse(value)),
    "aiw-apply-settings": value => {
      const preview = JSON.parse(value);
      showDialog("agent-confirm-settings", "应用录制参数", settingsChanges(preview),
        '<button type="button" class="btn primary" id="agent-apply-settings">确认应用</button>');
      dialog.querySelector("#agent-apply-settings").addEventListener("click", () => {
        dialog.close();
        Native.rpc("apply-assistant", { before: preview.before, changes: preview.changes }).then(settings => {
          Object.assign(state.settings, settings);
          refresh();
          toast("已应用录制参数");
        }, reportNativeError);
      }, { once: true });
    },
    "aiw-cut-preview": (value, thread) => { const row = candidate(thread, value); if (row) previewCandidate(row); },
    "aiw-cut-apply": (value, thread) => confirmCut(thread, Number(value)),
    "aiw-cut-export": async (value, thread) => {
      const cut = thread?.messages[Number(value)]?.cut;
      const ranges = cut?.targets.filter(row => row.selected !== false);
      if (!ranges?.length || new Set(ranges.map(item => item.id)).size < 2) return;
      const file = await Native.rpc("save-cut-sequence", { ranges, target: cut.target }).catch(reportNativeError);
      if (file) toast("多素材草稿已保存");
    },
    "aiw-cut-order": (value, thread) => {
      const [message, index, delta] = String(value).split(":").map(Number);
      const rows = thread?.messages[message]?.cut?.targets;
      if (!rows?.[index] || !rows[index + delta]) return;
      [rows[index], rows[index + delta]] = [rows[index + delta], rows[index]];
      persist();
      refresh();
    },
    "aiw-search-cut": (value, thread) => prepareFoundCut(thread, value),
    "aiw-refresh-workspace": (_, thread) => { if (thread) refreshWorkspace(thread.project, true); },
    "aiw-choose-workspace": (_, thread) => { if (thread) pickWorkspaceFolder("add", thread.project); },
    "aiw-workspace-project": path => { if (path) openNativeProject({ path, project: path }); },
    "aiw-locate": value => locate(...JSON.parse(value)),
    "aiw-open-editor": id => {
      const registration = registrations.get(id) || [...registrations.values()].find(entry => entry.nativeId === id);
      if (registration?.project) openNativeProject({ path: registration.project, project: registration.id });
      else if (id) openEditorProject(id);
    },
    "aiw-add": (_, thread) => { if (thread) materialDialog(thread); },
    "aiw-configure": (id, thread) => {
      const item = thread?.materials.find(entry => entry.id === id);
      if (item) materialDialog(thread, item);
    },
    "aiw-mark": (id, thread) => {
      const item = thread?.materials.find(entry => entry.id === id);
      if (!item) return;
      item.marked = !item.marked;
      persist();
      refresh();
    },
    "aiw-remove": (id, thread) => {
      if (!thread) return;
      thread.materials = thread.materials.filter(item => item.id !== id);
      persist();
      refresh();
    },
    "aiw-archive": id => {
      const thread = byId(id);
      if (!thread || thread.archived) return;
      thread.archived = true;
      persist();
      refresh();
    },
    "aiw-restore": id => {
      const thread = byId(id);
      if (!thread?.archived) return;
      thread.archived = false;
      persist();
      refresh();
      settingsDialog("conversations");
    },
    "aiw-delete": id => {
      const thread = byId(id);
      if (!thread?.archived) return;
      showDialog("agent-delete", "删除对话", `<p>删除「${esc(title(thread))}」？</p>`,
        b("aiw-archive-settings", "返回", null, { variant: "flat" }) +
        b("aiw-confirm-delete", "删除", "trash", { value: id, variant: "danger" }), { cancel: false });
    },
    "aiw-confirm-delete": id => {
      const thread = byId(id);
      if (!thread?.archived) return;
      cancelThreadJob(thread);
      ConversationCompact.cancel(thread);
      threads = threads.filter(entry => entry !== thread);
      persist();
      settingsDialog("conversations");
    },
    "aiw-archive-settings": () => settingsDialog("conversations"),
  };

  document.addEventListener("click", event => {
    const control = event.target.closest("[data-action^='aiw-']");
    if (!control || control.disabled) return;
    clickHandlers[control.dataset.action]?.(control.dataset.value, activeSurface(control), control);
  });

  document.addEventListener("aiworkspacepreview", event => {
    if (!event.target.closest(".agent-right-content") || !event.detail?.assetId) return;
    const { assetId } = event.detail;
    const layout = rightLayout();
    let tab = layout.tabs.find(item => item.page === "preview" && item.assetId === assetId);
    if (!tab) {
      tab = { id: crypto.randomUUID(), page: "preview", assetId };
      layout.tabs.push(tab);
    }
    selectRightTab(tab.id);
  });

  document.addEventListener("input", event => {
    const input = event.target;
    if (input.matches("[data-agent-form='compose'] textarea")) {
      const thread = activeSurface(input);
      if (thread) drafts.set(thread.id, input.value);
    } else if (input.matches("[data-agent-range]")) {
      const thread = activeSurface(input);
      thread.params.target = Number(input.value);
      input.closest(".agent-params").querySelector("output").textContent = input.value + " " + UI.text("秒");
    }
  });

  document.addEventListener("change", event => {
    const control = event.target;
    if (control.dataset.agentMontage) {
      const [message, index] = control.dataset.agentMontage.split(":").map(Number);
      const row = activeSurface(control)?.messages[message]?.targets[index];
      if (row) { row.selected = control.checked; persist(); }
      return;
    }
    if (control.dataset.agentCandidate) {
      const thread = activeSurface(control);
      const item = candidate(thread, control.dataset.agentCandidate);
      if (!item) return;
      if (control.type === "checkbox") item.selected = control.checked;
      else {
        const edge = control.dataset.edge;
        const value = control.valueAsNumber;
        if (!Number.isFinite(value) || value < item.scope_start || value > item.scope_end ||
          (edge === "start" ? item.end - value : value - item.start) < .02) {
          control.value = item[edge];
          return;
        }
        item[edge] = value;
      }
      persist();
      const cut = thread.messages[Number(control.dataset.agentCandidate.split(":")[0])].cut;
      const selected = cut.targets.filter(row => row.selected !== false);
      const duration = selected.reduce((total, row) => total + row.end - row.start, 0);
      const panel = control.closest(".agent-cut");
      panel.querySelector(".agent-cut-summary small").textContent = `${selected.length} 段 · ${duration.toFixed(2)} 秒`;
      const multiple = new Set(selected.map(row => row.id)).size > 1;
      const apply = panel.querySelector('[data-action="aiw-cut-apply"]');
      const save = panel.querySelector('[data-action="aiw-cut-export"]');
      if (apply) apply.disabled = !selected.length || multiple;
      if (save) save.disabled = !multiple;
      return;
    }
    if (control.matches("[data-agent-range]")) { persist(); return; }
    const field = control.dataset.field;
    if (!field?.startsWith("aiw-")) return;
    if (field === "aiw-material-asset") {
      const id = control.value;
      const form = control.form;
      const submit = document.querySelector('button[form="agent-material-form"]');
      if (submit) submit.disabled = true;
      assureDuration(lookupAsset(id)).then(asset => {
        if (!form.isConnected || control.value !== id) return;
        form.elements.from.value = "0";
        form.elements.from.max = asset.duration;
        form.elements.to.value = asset.duration;
        form.elements.to.max = asset.duration;
        if (submit) submit.disabled = false;
      }, reportNativeError);
      return;
    }
    const thread = activeSurface(control);
    if (!thread) return;
    if (field === "aiw-selected") {
      const item = thread.materials.find(entry => entry.id === control.dataset.id);
      if (!item) return;
      item.selected = control.checked;
      persist();
      refresh();
      return;
    }
    const key = field.slice(4);
    if (!Object.hasOwn(thread.params, key)) return;
    if (control.type === "number") {
      if (control.value.trim() === "" || !Number.isFinite(control.valueAsNumber) || !control.checkValidity()) {
        control.value = thread.params[key];
        return;
      }
      thread.params[key] = control.valueAsNumber;
    } else thread.params[key] = control.type === "checkbox" ? control.checked : control.value;
    persist();
    if (key === "range") refresh();
  });

  document.addEventListener("keydown", event => {
    if (!event.target.matches(".agent-tab-button")) return;
    const { tabs } = rightLayout();
    const index = tabs.findIndex(tab => tab.id === event.target.dataset.value);
    if (index < 0 || !["ArrowLeft", "ArrowRight", "Home", "End", "Delete"].includes(event.key)) return;
    event.preventDefault();
    if (event.key === "Delete") closeRightTab(tabs[index].id);
    else {
      const indices = { Home: 0, End: tabs.length - 1,
        ArrowLeft: (index + tabs.length - 1) % tabs.length, ArrowRight: (index + 1) % tabs.length };
      selectRightTab(tabs[indices[event.key]].id);
    }
  });

  document.addEventListener("keydown", event => {
    if (event.key !== "Enter" || event.shiftKey || event.isComposing || event.keyCode === 229 ||
      !event.target.matches("[data-agent-form='compose'] textarea")) return;
    event.preventDefault();
    event.target.form.requestSubmit();
  });

  document.addEventListener("submit", event => {
    const form = event.target;
    if (!form.dataset.agentForm) return;
    event.preventDefault();
    const thread = activeSurface(form);
    if (!thread) return;
    const fields = form.elements;
    if (form.dataset.agentForm === "compose") {
      send(thread, fields.prompt.value, !!form.closest("#assistant"));
      return;
    }
    if (form.dataset.agentForm !== "material") return;
    const asset = lookupAsset(fields.asset.value);
    const from = fields.from.valueAsNumber;
    const to = fields.to.valueAsNumber;
    if (!asset || !Number.isFinite(from) || !Number.isFinite(to) || from < 0 || from >= to || to > asset.duration) {
      toast("开始时间应早于结束时间，且都在视频范围内。");
      return;
    }
    const current = thread.materials.find(item => item.id === form.dataset.item);
    const item = current || material(asset, from, to);
    Object.assign(item, { from, to, name: fields.name.value.trim(), note: fields.note.value.trim() });
    if (!current) thread.materials.push(item);
    dialog.close();
    persist();
    refresh();
  });

  for (const thread of threads) thread.pending = null;

  return { page, sidebar, currentProject, resetSession, archivedSettings };
})();
