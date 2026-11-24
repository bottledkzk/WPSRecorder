"use strict";

const AgentWorkspacePages = (() => {
  const { escape: esc, glyph } = UI;
  const sortNames = new Intl.Collator("zh-CN", { numeric: true });
  const sessions = new WeakMap();
  let active;

  function sortEntries(entries) {
    return entries.sort((a, b) => Number(!!b.children) - Number(!!a.children) || sortNames.compare(a.name, b.name));
  }

  function fileTree(roots) {
    return roots.map((root, index) => {
      const base = { name: root.name, key: root.id ?? index, children: new Map() };
      for (const file of root.files || []) {
        const segments = file.path.split(/[\\/]/).filter(Boolean);
        const name = segments.pop();
        if (!name) continue;
        let folder = base;
        for (const segment of segments) {
          if (!folder.children.has(segment)) folder.children.set(segment, {
            name: segment, key: folder.key + "/" + segment, children: new Map(),
          });
          folder = folder.children.get(segment);
        }
        if (file.directory) {
          if (!folder.children.has(name)) folder.children.set(name, { name, key: folder.key + "/" + name, children: new Map() });
        } else folder.children.set(name, { name, file });
      }
      return base;
    });
  }

  function fileRow({ name, file }) {
    const image = /\.(?:avif|gif|jpe?g|png|svg|webp)$/i.test(name);
    const video = /\.(?:avi|mkv|mov|mp4|m4v|webm)$/i.test(name);
    const icon = image ? "image" : video ? "film" : "text";
    const content = `<span class="agent-workspace-file-icon ${icon}">${glyph(icon)}</span><span class="truncate">${esc(name)}</span>`;
    const attrs = `class="agent-workspace-file" title="${esc(file.path)}"`;
    if (file.projectPath || file.projectId) {
      return `<button type="button" ${attrs} data-action="${file.projectPath ? "aiw-workspace-project" : "aiw-open-editor"}"
        data-value="${esc(file.projectPath || file.projectId)}">${content}</button>`;
    }
    if (file.assetId) {
      return `<button type="button" ${attrs} data-workspace-video="${esc(file.assetId)}">${content}</button>`;
    }
    return `<span ${attrs}>${content}</span>`;
  }

  function treeRows(nodes, search, folders, rootLevel = true) {
    return sortEntries([...nodes]).map(node => {
      if (node.file) {
        if (search && !node.name.toLocaleLowerCase().includes(search)) return "";
        return `<li>${fileRow(node)}</li>`;
      }
      const matches = search && node.name.toLocaleLowerCase().includes(search);
      const rows = treeRows(node.children.values(), matches ? "" : search, folders, false);
      if (search && !matches && !rows) return "";
      const expanded = search ? !!rows : (folders.get(String(node.key)) ?? rootLevel);
      return `<li><details data-workspace-folder="${esc(node.key)}"${expanded ? " open" : ""}>
        <summary title="${esc(node.name)}">${glyph("right")}<span class="truncate">${esc(node.name)}</span></summary>
        <ul>${rows}</ul></details></li>`;
    }).join("");
  }

  function fileTreeBody(view) {
    const query = view.session.filter.trim().toLocaleLowerCase();
    const rows = treeRows(view.tree, query, view.session.folders);
    if (rows) return `<ul class="agent-workspace-roots">${rows}</ul>`;
    return `<p class="agent-workspace-empty">${query ? "没有匹配的文件" : view.roots.length ? "目录为空" : "请选择工作区文件夹"}</p>`;
  }

  function workspace(view) {
    return `<div class="agent-workspace-search">${glyph("search")}
      <input type="search" aria-label="筛选文件" placeholder="筛选文件…" value="${esc(view.session.filter)}">
      ${view.roots.length ? "" : UI.iconButton("aiw-choose-workspace", "添加工作区文件夹", "plus")}
      <button type="button" class="btn flat icon-btn" data-action="aiw-refresh-workspace" title="刷新工作区" aria-label="刷新工作区">${glyph("refresh")}</button></div>
      ${view.roots.some(root => root.loading && !root.files) ? '<p class="agent-workspace-empty" role="status">正在读取目录…</p>' : ""}
      ${view.roots.some(root => root.truncated || root.error) ? `<p class="agent-workspace-empty" role="status">${esc(view.roots.find(root => root.error)?.error || "文件较多，请添加子目录以查看其余文件。")}</p>` : ""}
      <div class="agent-workspace-tree" aria-label="工作区文件">${fileTreeBody(view)}</div>`;
  }

  function videoPlayer(view) {
    const video = view.videos.find(item => item.id === view.session.video);
    if (!video?.src) {
      return `<p class="agent-workspace-empty">${video ? "请重新选择包含该视频的文件夹" : "暂无可预览的视频"}</p>`;
    }
    return `<video controls playsinline preload="metadata" aria-label="${esc(video.title ?? video.name)}"
      src="${esc(video.src)}"${video.poster ? ` poster="${esc(video.poster)}"` : ""}></video>
      <p class="agent-workspace-empty" role="status" hidden>无法播放此视频</p>`;
  }

  function preview(view) {
    const { session, videos, tab } = view;
    if (tab.assetId && tab.assetId !== session.openedVideo) {
      session.openedVideo = tab.assetId;
      session.video = tab.assetId;
      session.time = 0;
    }
    if (!videos.some(video => video.id === session.video)) {
      session.video = videos[0]?.id;
      session.time = 0;
    }
    return `<div class="agent-workspace-preview-select"><select aria-label="选择预览视频"${videos.length ? "" : " disabled"}>
      ${videos.length ? videos.map(video => `<option value="${esc(video.id)}"${video.id === session.video ? " selected" : ""}>
        ${esc(video.title ?? video.name)}</option>`).join("") : "<option>选择视频</option>"}</select></div>
      <div class="agent-workspace-player">${videoPlayer(view)}</div>`;
  }

  function terminal(view) {
    return `<div class="agent-terminal-toolbar"><small>WPS Recorder CLI</small>
      <button type="button" class="btn flat mini" data-workspace-clear>清屏</button></div>
      <pre class="agent-terminal-output" role="log" aria-live="polite">${esc(view.session.output.join("\n"))}</pre>
      <form class="agent-terminal-prompt"><label><b>$</b><input aria-label="终端命令" type="text"
        autocomplete="off" spellcheck="false" placeholder="--help"></label></form>`;
  }

  function render(page, tab, { roots = [], videos = [], workspace: workspaceId, folder } = {}) {
    if (!sessions.has(tab)) sessions.set(tab, { filter: "", folders: new Map(), output: [], time: 0 });
    active = { tab, workspace: workspaceId, folder, roots, tree: page === "workspace" ? fileTree(roots) : [], videos, session: sessions.get(tab) };
    const renderer = { workspace, preview, terminal }[page];
    return `<div class="agent-workspace-page agent-workspace-${page}" data-agent-workspace-tab="${esc(tab.id)}">${renderer(active)}</div>`;
  }

  function context(event) {
    const panel = event.target.closest?.("[data-agent-workspace-tab]");
    if (panel?.dataset.agentWorkspaceTab !== active?.tab.id) return null;
    return panel;
  }

  document.addEventListener("input", event => {
    const panel = context(event);
    if (!panel || !event.target.matches(".agent-workspace-search input")) return;
    active.session.filter = event.target.value;
    panel.querySelector(".agent-workspace-tree").innerHTML = fileTreeBody(active);
  });

  document.addEventListener("toggle", event => {
    const panel = context(event);
    if (panel && event.target.matches("details[data-workspace-folder]") && !active.session.filter.trim()) {
      active.session.folders.set(event.target.dataset.workspaceFolder, event.target.open);
    }
  }, true);

  document.addEventListener("click", event => {
    const panel = context(event);
    if (!panel) return;
    const video = event.target.closest("[data-workspace-video]");
    if (video) {
      video.dispatchEvent(new CustomEvent("aiworkspacepreview", { bubbles: true, detail: { assetId: video.dataset.workspaceVideo } }));
    }
    if (event.target.closest("[data-workspace-clear]")) {
      active.session.output = [];
      panel.querySelector(".agent-terminal-output").textContent = "";
      panel.querySelector(".agent-terminal-prompt input").focus();
    }
  });

  document.addEventListener("change", event => {
    const panel = context(event);
    if (!panel || !event.target.matches(".agent-workspace-preview-select select")) return;
    active.session.video = event.target.value;
    active.session.time = 0;
    panel.querySelector(".agent-workspace-player").innerHTML = videoPlayer(active);
  });

  document.addEventListener("timeupdate", event => {
    if (context(event) && event.target.matches(".agent-workspace-player video")) {
      active.session.time = event.target.currentTime;
    }
  }, true);

  document.addEventListener("loadedmetadata", event => {
    if (context(event) && event.target.matches(".agent-workspace-player video") && active.session.time) {
      event.target.currentTime = Math.min(active.session.time, event.target.duration);
    }
  }, true);

  document.addEventListener("error", event => {
    if (context(event) && event.target.matches(".agent-workspace-player video")) {
      event.target.hidden = true;
      event.target.nextElementSibling.hidden = false;
    }
  }, true);

  document.addEventListener("submit", async event => {
    const panel = context(event);
    if (!panel || !event.target.matches(".agent-terminal-prompt")) return;
    event.preventDefault();
    const input = event.target.querySelector("input");
    const command = input.value.trim();
    if (!command) return;
    input.value = "";
    const view = active;
    if (command === "clear") view.session.output = [];
    else {
      view.session.output = [...view.session.output, "$ " + command].slice(-100);
      try {
        const result = await Native.rpc("cli", { input: command, workspace: view.workspace, folder: view.folder });
        view.session.output.push(result.text.trim() || "退出码 " + result.code);
      } catch (error) { view.session.output.push(error.message); }
    }
    if (active?.tab !== view.tab) return;
    const log = document.querySelector("[data-agent-workspace-tab='" + CSS.escape(view.tab.id) + "'] .agent-terminal-output");
    if (log) {
      log.textContent = view.session.output.join("\n");
      log.scrollTop = log.scrollHeight;
    }
  });

  return { render };
})();
