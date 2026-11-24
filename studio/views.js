"use strict";

const Views = (() => {
  const { button: b, iconButton: ib, glyph: g, escape: esc, time, head, tabs, select, pct, jpg } = UI;
  const tr = label => esc(UI.text(label));
  const pop = (label, items) => `<details class="shell-menu"><summary class="btn flat">${esc(UI.text(label))}</summary>
    <div class="popup" aria-label="${esc(UI.text(label))}">${items.map(([action, name, icon, shortcut, value]) =>
      b(action, name, icon, { variant: "flat", shortcut, value })).join("")}</div></details>`;
  const currentAsset = () => assets.find(asset => asset.id === state.asset) ?? emptyAsset;
  const editorAsset = () => editorProjectAsset() ?? emptyAsset;

  function titlebar() {
    const nav = [
      ["record", "录制工作台", "video"], ["review", "回看与管理", "film"],
      ["editor", "剪辑工坊", "scissors"], ["ai", "智能 AI 助手", "spark"],
    ];
    const editing = state.page === "editor";
    const tools = [["schedule", "预约录制", "calendar"], ["annotate", "讲解工具", "pen"],
      ["recover-recording", "恢复录制素材…", "undo"]];
    if (state.page === "record") tools.push(["screenshot", "截取当前画面", "camera"]);
    if (editing) tools.push(["editor-tab", "字幕与校对", "text", null, "subtitle"],
      ["editor-tab", "配音与旁白", "mic", null, "dub"]);
    const file = [
      ["open-local", "打开本地视频回看", "folder", "Ctrl+O"],
      ["open-editor-project", "打开剪辑工程…", "folder"],
      ["save", "保存剪辑工程", "save", "Ctrl+S"],
      ["export", editing ? "导出成片…" : "导出／格式转换…", "download", "Ctrl+E"],
    ];
    if (editing) file.push(["export", "导出选中片段…", "scissors", null, "clip"]);
    if (state.page === "review" && currentAsset().id) file.push(["reveal-media", "打开视频目录", "folder"],
      ["remove-video", "删除当前视频…", "trash"]);
    file.push(["open-directory", "打开录制目录", "disk"]);
    const pane = assistantPane();
    const view = [["float-toggle", state.telemetry.floating ? "隐藏悬浮条" : "显示悬浮条", "float"],
      ["minimal-recorder", "极简录制", "video"]];
    if (pane) view.push(["sidebar-ai", pane.open ? "关闭侧栏助手" : "打开侧栏助手", "bot"]);
    view.push(["app-full", state.telemetry.maximized ? "退出全屏" : "全屏显示", "full"]);
    return `<div class="hstack">
      <div class="brand"><img src="assets/studio.svg" alt=""><strong>WPS Recorder</strong><span class="studio-mark">STUDIO</span></div>
      <span class="thin-rule"></span><nav class="hstack menu-group" aria-label="${esc(UI.text("应用菜单"))}">
      ${pop("文件", file)}
      ${pop("工具", tools)}
      ${pop("视图", view)}
      ${b("settings", "设置", null, { variant: "flat shell-settings" })}
      ${pop("帮助", [["help", "软件说明", "book"], ["shortcuts", "快捷键", "link"],
        ["settings", "主题设置", "palette", "", "theme"],
        ["settings", "模型服务", "chip", "", "models"]])}</nav></div>
      <nav class="segmented workspace-nav" aria-label="${esc(UI.text("工作区"))}">${nav.map(([id, label, icon]) =>
        b("workspace", label, icon, { value: id, variant: "flat", pressed: state.page === id })).join("")}</nav>
      <div class="hstack title-actions">
        ${b("schedule", "预约", "calendar", { optional: true })}
        ${b("float-toggle", "悬浮条", "float", { optional: true, pressed: Boolean(state.telemetry.floating) })}
        ${b("export", "导出成片", "download", { variant: "primary", optional: true })}
        <div class="hstack caption">
          ${ib("minimize", "最小化", "minus", { variant: "flat" })}
          ${ib("app-full", state.telemetry.maximized ? "还原窗口" : "最大化",
            state.telemetry.maximized ? "restore" : "square", { variant: "flat" })}
          ${ib("close-app", "关闭", "close", { variant: "flat danger" })}
        </div>
      </div>`;
  }

  function status() {
    const active = state.record.phase === "recording" || state.record.phase === "paused";
    const t = state.telemetry;
    const label = { recording: "录制中", paused: "已暂停", saving: "保存中" }[state.record.phase] || "就绪";
    const frames = tr("写入 %1 帧").replace("%1", `<span id="frames">${t.frames || 0}</span>`);
    return `<div class="hstack"><i class="dot ${active ? "live" : "green"}"></i>
      <span>${tr(label)}</span>
      <span class="status-item mono">REC <span data-clock>${time(state.record.seconds, true)}</span></span>
    </div><div class="hstack status-right mono">
      <span class="status-item optional-status">${tr((t.encoder || state.settings.encoder).replace("NVIDIA ", ""))}</span>
      <span class="status-item">${resolutionLabel()}</span>
      <span class="status-item">${g("wave")} ${Number(t.fps || 0).toFixed(1)} / ${state.settings.fps} fps</span>
      <span class="status-item optional-status">${frames}</span>
      <span class="status-item optional-status">${tr("补帧")} ${t.duplicates || 0}</span>
      <span class="status-item optional-status">${state.settings.bitrate} kbps</span>
      <span class="status-item" title="WPS Recorder 的 CPU 占用" aria-label="WPS Recorder CPU">${g("chip")} CPU ${Number(t.cpu || 0).toFixed(1)}%</span>
      <span class="status-item optional-status" title="剩余存储空间">${g("disk")} ${t.disk >= 0 ? Math.floor(t.disk) + " GB" : "—"}</span>
    </div>`;
  }

  const full = (page, variant = "flat mini") => ib("fullscreen", "无边框全屏预览", "full", { value: page, variant });
  const leaveFull = () => b("leave-full", "退出全屏", "close", { variant: "flat mini fullscreen-exit" });
  const assistantToggle = () => ib("sidebar-ai", "侧栏助手", "bot", { pressed: Boolean(assistantPane()?.open) });
  const splitHandle = (name, title, horizontal = false) => `<div class="splitter${horizontal ? " horizontal" : ""}" data-divider="${name}"
    tabindex="0" role="separator" aria-label="${title}" aria-orientation="${horizontal ? "horizontal" : "vertical"}"></div>`;
  const formSelect = (id, name, choices, value) => `<label for="${id}">${esc(UI.text(name))}</label>${select(id, name, choices, value, `id="${id}"`)}`;
  const checkbox = (id, label, on) => `<label class="hstack"><input type="checkbox" data-field="${id}" ${on ? "checked" : ""}>${tr(label)}</label>`;

  function captureMedia() {
    if (!activeScene().sources.some(item => item.visible)) return `<span class="empty-preview soft">${tr("无信号")}</span>`;
    const frame = state.liveFrame;
    return `<img id="capture-image" alt="当前画面源预览"${frame?.src ? ` src="${esc(frame.src)}"` : " hidden"}>
      <span class="empty-preview soft" id="capture-notice"${frame?.src ? " hidden" : ""} role="status">${tr(state.previewNotice || "正在连接画面源…")}</span>`;
  }

  function annotationTools() {
    const a = state.annotation;
    if (!a.open) return "";
    const grip = `<button type="button" class="btn flat mini icon-btn annotation-grip"
      aria-label="移动讲解工具条" title="移动讲解工具条">${g("grip")}</button>`;
    if (a.collapsed) {
      return `<div class="annotation-toolbar annotation-compact" role="toolbar" aria-label="讲解工具条">
        ${grip}${ib("annotation-collapse", "展开讲解工具条", "up")}</div>`;
    }
    const options = [
      ["cursor", "常规鼠标", "pointer"], ["pen", "画笔", "nib"], ["arrow", "箭头", "arrow"],
      ["rect", "矩形", "square"], ["highlight", "荧光高亮", "highlighter"], ["text", "文字", "text"],
      ["eraser", "橡皮擦", "eraser"],
      ["blur", "模糊马赛克", "blur"], ["mask", "实色遮挡", "mask"],
    ];
    const setting = key => {
      const { label, icon, items } = annotationOptions[key];
      const current = items.find(([value]) => value === a[key])?.[1];
      return ib("annotation-menu", current ? label + " · " + current : label, icon, { value: key });
    };
    return `<div class="annotation-toolbar" role="toolbar" aria-label="讲解工具条">
      <div class="annotation-row">
        ${grip}
        <div class="annotation-group annotation-instruments">
          ${options.map(([id, label, icon]) => ib("annotation-tool", label, icon, { value: id, pressed: a.tool === id })).join("")}
        </div>
        <div class="annotation-group">
          <input type="color" data-field="ink" aria-label="标注颜色" title="标注颜色" value="${a.color}">
          ${setting("size")}
          ${a.tool === "blur" ? setting("blur") : ""}
        </div>
      </div>
      <div class="annotation-row annotation-secondary">
        <div class="annotation-group">
          ${ib("annotation-effect", "聚光", "spark", { value: "spotlight", pressed: a.effect === "spotlight" })}
          ${ib("annotation-effect", "放大", "search", { value: "magnifier", pressed: a.effect === "magnifier" })}
          ${ib("annotation-whiteboard", "白板", "board", { pressed: a.whiteboard })}
          ${setting("clickStyle")}
        </div>
        <div class="annotation-group annotation-history">
          ${ib("annotation-undo", "撤销标注", "undo", { disabled: !a.undo.length })}
          ${ib("annotation-redo", "重做标注", "redo", { disabled: !a.redo.length })}
          ${ib("annotation-clear", "清空标注", "trash", { disabled: !a.strokes.length })}
          ${ib("annotation-collapse", "收起讲解工具条", "chevron")}
          ${ib("annotate", "退出讲解模式", "close", { variant: "flat mini danger" })}
        </div>
      </div>
    </div>`;
  }

  function pipControls() {
    const camera = overlayCamera();
    if (!camera) return "";
    const { x, y, size } = cameraPlacement(camera);
    const bounds = `left:${x * 100}%;top:${y * 100}%;width:${size * 100}%;height:${size * 100}%`;
    const placement = activeSource() === camera && !state.annotation.open
      ? `<button type="button" class="pip-placement" aria-label="调整画中画位置" title="拖动画中画"
        style="${bounds}"${state.telemetry.cameraReady ? "" : " hidden"}></button>` : "";
    const error = state.telemetry.cameraError;
    return `${placement}<span id="capture-camera-notice" class="capture-camera-notice" role="status" style="${bounds}"
      title="${esc(error || "")}"${!state.liveFrame || state.telemetry.cameraReady ? " hidden" : ""}>
      <span>${esc(cameraPreviewNotice())}</span></span>`;
  }

  function recordMonitor() {
    const item = activeSource();
    const type = item && sourceCatalog[item.type];
    const right = `<div class="hstack">${assistantToggle()}${b("annotate", "讲解", "pen", { variant: "flat mini", pressed: state.annotation.open })}
      ${full("record")}${leaveFull()}</div>`;
    return `<section class="panel record-monitor" data-screen="record">
      ${head("实时预览", "monitor", right)}
      <div class="record-canvas-wrap">
        <div class="media-surface" id="record-canvas">
          <div class="media-fit${state.zoom ? " zoomed" : ""}"><div class="media-frame" id="capture-frame">
            ${captureMedia()}
            ${pipControls()}
          </div></div>
        </div>
      </div>
      <div class="hstack preview-toolbar">
        <div class="hstack source-control">${item ? `
          <button type="button" class="btn mini quick-target" data-action="quick-target"
            title="${esc(targetFor(item)[0])}" aria-haspopup="listbox" aria-expanded="false">${g(type.icon)}<span class="truncate">${esc(targetFor(item)[0])}</span>${g("chevron")}</button>
          ${b("source-properties", "设置", "cog", { variant: "mini" })}` : ""}
        </div>
        <div class="hstack zoom-control">
          ${ib("preview-zoom", "缩小预览", "minus", { value: -1 })}
          <output class="mono" id="preview-ratio"></output>
          ${ib("preview-zoom", "放大预览", "plus", { value: 1 })}
          ${select("preview-fit", "画面适配", [[0, "适应窗口"], [100, "原始尺寸"], [150, "150%"], [200, "200%"],
          ...([0, 100, 150, 200].includes(state.zoom) ? [] : [[state.zoom, "自定义缩放"]])], state.zoom)}
        </div>
      </div>
    </section>`;
  }

  function outputPath(id) {
    const path = state.settings.outputPath;
    const title = state.settings.outputPathIsFolderName ? `已选目录 ${path}（浏览器只提供名称）` : path;
    return `<label for="${id}">${tr("路径")}</label><div class="form-row output-path">
      <input id="${id}" type="text" value="${esc(path)}" title="${esc(title)}" data-output-path readonly>
      ${b("choose-output-path", "浏览…", null, { variant: "mini" })}</div>`;
  }

  const dockHead = (label, icon, right) => head(label, icon, right, "",
    ' data-dock-handle tabindex="0" aria-keyshortcuts="Alt+ArrowLeft Alt+ArrowRight" title="拖动模块换位"');

  function recordSettings() {
    const s = state.settings;
    return `<section class="panel record-settings" data-dock="output">${dockHead("录制设置", "cog")}
      <div class="scroll-body"><div class="form">
        ${formSelect("resolution", "分辨率", [[0, "自动"], [720, "1280 × 720"], [1080, "1920 × 1080"], [1440, "2560 × 1440"], [2160, "3840 × 2160"], [-1, "自定义"]], s.resolution)}
        ${s.resolution === -1 ? `<span></span><div class="hstack custom-record-size">
          <input type="number" min="160" max="7680" step="2" data-field="capture-width" aria-label="录制宽度" value="${s.width}">
          <span>×</span>
          <input type="number" min="120" max="4320" step="2" data-field="capture-height" aria-label="录制高度" value="${s.height}"></div>` : ""}
        ${formSelect("fps", "帧率", [30, 60, 90, 144].map(fps => [fps, fps + " fps"]), s.fps)}
        <label for="bitrate">${tr("码率")}</label><div class="form-row bitrate-row"><input id="bitrate" type="number" min="500" max="150000" step="100"
        data-field="bitrate" value="${s.bitrate}" aria-label="视频码率 kbps"><small class="mono">kbps</small>
        ${b("reset-rate", "默认", "refresh", { variant: "mini", optional: true })}</div>
        ${formSelect("encoder", "编码器", ["自动", "NVIDIA NVENC H.264", "Intel QSV H.264", "AMD AMF H.264", "CPU · x264"], s.encoder)}
        ${formSelect("color", "色彩", ["SDR", "HDR", "SDR 转 HDR", "HDR 转 SDR"], s.color)}
        ${formSelect("format", "格式", ["MP4", "MKV", "MOV", "AVI", "FLV"], s.format)}
        ${outputPath("record-output-path")}
      </div></div></section>`;
  }

  function scenesDock() {
    const form = `<form class="scene-edit" data-form="scene">
      <input type="text" name="scene" aria-label="场景名称" placeholder="场景名称" maxlength="32" required
        value="${state.sceneEdit === "rename" ? esc(activeScene().name) : ""}">
      <button class="btn flat mini icon-btn" type="submit" aria-label="保存" title="保存">${g("check")}</button>
      ${ib("scene-cancel", "取消", "close")}</form>`;
    return `<section class="panel scenes-dock" data-dock="scene">
      ${dockHead("场景", "layers")}
      <div class="scroll-body"><div class="dock-list">${state.scenes.map((scene, index) =>
        state.sceneEdit === "rename" && state.scene === index ? form :
        b("choose-scene", scene.name, "layers", { value: index, variant: "flat mini scene-choice", pressed: state.scene === index }))
        .join("")}${state.sceneEdit === "new" ? form : ""}</div></div>
      <div class="panel-foot dock-actions">
        ${ib("scene-new", "添加场景", "plus")}
        ${ib("scene-rename", "重命名场景", "pen")}
        ${ib("remove-scene", "删除当前场景", "trash", { disabled: state.scenes.length <= 1 })}
      </div></section>`;
  }

  function sourcesDock() {
    const scene = activeScene();
    const rows = scene.sources.slice().reverse().map(item => {
      const type = sourceCatalog[item.type];
      const label = targetFor(item)[0];
      const name = label.split(" · ")[0];
      return `<div class="source-row${scene.selected === item.id ? " selected" : ""}">
        <button type="button" class="source-pick hstack grow" data-action="source-select" data-value="${item.id}"
          aria-pressed="${scene.selected === item.id}" title="${esc(label)}">
          <span class="source-icon${item.type === "camera" ? " camera" : ""}">${g(type.icon)}</span>
          <span class="truncate"><strong>${esc(name)}</strong></span>
        </button>${ib("source-visible", item.visible ? "隐藏 " + name : "显示 " + name, item.visible ? "eye" : "hidden", { value: item.id })}</div>`;
    }).join("");
    return `<section class="panel source-dock" data-dock="source">${dockHead("源", "monitor")}
      <div class="scroll-body"><div class="dock-list">${rows}</div></div>
      <div class="panel-foot dock-actions">
        ${ib("source-picker", "添加画面来源", "plus")}
        ${ib("remove-source", "删除选中来源", "trash", { disabled: !activeSource() })}
      </div></section>`;
  }

  function mixerDock() {
    const actions = `<div class="hstack mixer-head-actions">${select("audio-mode", "录音轨道模式", mixerTrackModes(), state.settings.audioMode)}
      ${ib("mixer-picker", "添加音频来源", "plus", { disabled: Object.keys(state.audio).length >= 8 || state.record.phase !== "idle" })}</div>`;
    return `<section class="panel mixer-dock" data-dock="mixer">
      ${dockHead("混音器", "wave", actions)}
      <div class="inset scroll-body">${Object.entries(state.audio).map(([id, channel]) => {
        const { name, icon, peak, db, optional } = mixerSources[id];
        const denoise = id === "mic" ? state.settings.denoise : channel.denoise;
        const level = channel.muted ? 0 : peak * channel.volume / 100;
        const muteIcon = channel.muted ? (icon === "mic" ? "mic-off" : "mute") : icon;
        return `<div class="audio-channel">
          <div class="hstack channel-name">
          ${ib("mixer-mute", channel.muted ? "取消" + name + "静音" : name + "静音", muteIcon,
            { variant: "flat mini mixer-mute " + (icon === "mic" ? "input-channel" : "output-channel"),
              pressed: channel.muted, value: id })}
          <span class="truncate" title="${tr(name)}">${tr(name)}</span>
          <span class="db mono" data-db="${id}">${channel.muted ? tr("静音") : db}</span>
          ${icon === "mic" ? b("denoise", "降噪", "spark", { value: id, variant: "flat mini noise-toggle", pressed: denoise, optional: true }) : ""}
          ${optional ? ib("mixer-remove", "移除" + name, "trash", { value: id }) : ""}</div>
          <div class="mixer-meter" data-meter="${id}" role="meter" aria-label="${esc(name)}电平"
            aria-valuemin="0" aria-valuemax="100" aria-valuenow="${Math.round(level)}" style="--meter-level:${level}%"></div>
          <div class="mixer-level">
          <input type="range" min="0" max="100" value="${channel.volume}" data-range="gain" data-channel="${id}" aria-label="${esc(name)}音量">
          <output class="mono" data-gain="${id}">${channel.volume}%</output></div>
        </div>`;
      }).join("")}</div>
    </section>`;
  }

  function controlDock() {
    const r = state.record;
    const active = ["recording", "paused"].includes(r.phase);
    const stoppingAllowed = active || r.phase === "starting";
    const label = r.phase === "countdown" ? `<span id="countdown-left">${r.countdown}</span> ${tr("秒")}` :
      tr({ starting: "启动中", recording: "录制中", paused: "已暂停", saving: "保存中" }[r.phase] || "");
    return `<section class="panel control-dock" data-dock="control">${dockHead("录制控制", "video", ib("schedule", "预约录制", "calendar"))}
      <div class="inset stack scroll-body">
        <div class="hstack record-time">
          <i class="dot${r.phase === "idle" ? " green" : " live"}"${label ? ' aria-hidden="true"' : ` role="img" aria-label="${tr("就绪")}"`}></i>
          <time class="record-clock" data-clock>${time(r.seconds, true)}</time>
          ${label ? `<span class="hstack rec-state${active ? " is-live" : ""}">${label}</span>` : ""}
        </div>
        <div class="hstack record-command">${b("record-action", r.phase === "saving" ? "保存中" : stoppingAllowed ? "停止录制" : r.phase === "countdown" ? "取消倒计时" : "开始录制",
          stoppingAllowed ? "stop" : "record", { variant: "record-primary grow", disabled: r.phase === "saving" })}
          <button type="button" class="btn icon-btn record-options-toggle" data-action="countdown" title="${tr("录制倒计时")}"
            data-menu-align="end" aria-label="${tr("录制倒计时")}" aria-haspopup="listbox"
            aria-expanded="false"${r.phase !== "idle" ? " disabled" : ""}>${g("chevron")}</button>
        </div>
        <div class="record-secondary">
          ${b("pause-record", r.phase === "paused" ? "继续" : "暂停", r.phase === "paused" ? "play" : "pause", { disabled: !active, optional: true })}
          ${b("screenshot", "截图", "camera", { optional: true })}${b("record-mark", "打点", "flag", { disabled: !active, optional: true })}</div>
      </div></section>`;
  }

  function recordPage() {
    const panels = { scene: scenesDock, source: sourcesDock, mixer: mixerDock, output: recordSettings, control: controlDock };
    return `<div class="page record-page" style="--dock-height:${state.height.docks}px">
      ${recordMonitor()}
      ${splitHandle("docks", "调整录制模块高度", true)}
      <div class="register-docks" style="--dock-widths:${dockColumns()}">
        ${state.dockOrder.map((id, i, order) => panels[id]() +
          (i < order.length - 1 ? splitHandle("dock-" + i, dockDividerLabel(i)) : "")).join("")}
      </div>
    </div>`;
  }

  function mediaItem(asset, action, active, long = true) {
    const thumbnail = asset.poster ? `<img alt="" src="${esc(asset.poster)}">` : `<span class="source-icon">${g("film")}</span>`;
    return `<button type="button" class="media-item${active ? " selected" : ""}" data-action="${action}" data-value="${asset.id}">
      <div class="media-thumb">${thumbnail}<time>${time(asset.duration)}</time></div><div class="media-summary">
        <strong class="truncate" title="${esc(asset.title)}">${esc(asset.title)}</strong>
        ${long ? `<small class="truncate">${esc(asset.name)}</small><small class="hstack">
          <span>${asset.kind === "recoverable" ? "待恢复" : esc(asset.date)}</span>
          ${asset.marks.length ? `<span class="hstack flag-count" title="打点数量">${g("flag")}${asset.marks.length}</span>` : ""}
        </small>` : `<small class="mono">${time(asset.duration)} · ${asset.width ? asset.width + "×" + asset.height : "—"}</small>`}
      </div></button>`;
  }

  function libraryItems() {
    const query = state.review.query.trim().toLowerCase();
    const filtered = assets.filter(asset =>
      (state.review.filter === "all" || asset.kind === state.review.filter) && asset.name.toLowerCase().includes(query));
    return filtered.length ? filtered.map(asset => mediaItem(asset, "review-select", asset.id === state.asset)).join("")
      : '<p class="empty">没有匹配的视频</p>';
  }

  function seekMarkers(kind, duration, marks) {
    return marks.filter(mark => mark.at >= 0 && mark.at <= duration).map(mark => `<button type="button"
        class="seek-marker" data-action="${kind}-seek" data-value="${mark.at}" title="${esc(mark.name)} · ${time(mark.at)}"
        aria-label="${esc(mark.name)}" style="--position:${pct(mark.at, duration)}"></button>`).join("");
  }

  function seeker(kind, at, duration, marks) {
    const review = kind === "review";
    const limit = Number.isFinite(duration) && duration > 0 ? duration : 0;
    const markers = seekMarkers(kind, duration, marks);
    return `<div class="seekbar review-seekbar" style="--progress:${pct(at, limit)}">
      <span class="seek-rail" aria-hidden="true"></span>
      <input type="range" data-seek="${kind}" min="0" max="${limit}" step="any" value="${at}"${limit ? "" : " disabled"}
        aria-label="${review ? "回看播放进度" : "剪辑预览进度"}">
      <div class="review-seek-markers">${markers}</div>
    </div>`;
  }

  function playerControls(kind, at, duration, marks) {
    const { playing, volume, speed } = state[kind];
    return `<div class="player-controls">${seeker(kind, at, duration, marks)}<div class="hstack transport">
      ${ib(kind + "-prev", "上一章节", "previous")}
      ${ib(kind + "-play", playing ? "暂停播放" : "播放", playing ? "pause" : "play", { variant: "primary play-key" })}
      ${ib(kind + "-next", "下一章节", "next")}
      <span class="transport-time mono"><time data-player-time="${kind}">${time(at)}</time> <span class="soft" data-player-duration="${kind}">/ ${time(duration)}</span></span>
      <span class="grow"></span>
      ${ib("player-mute", volume ? "静音" : "取消静音", volume ? "volume" : "mute", { value: kind })}
      <input class="player-gain" type="range" min="0" max="100" value="${volume}" data-range="${kind}-volume" aria-label="播放音量">
      ${select("play-speed", "播放速度", [0.5, 0.75, 1, 1.25, 1.5, 2].map(s => [s, s + "×"]), speed)}
      ${full(kind)}
    </div></div>`;
  }

  function chapters(marks, kind) {
    if (!marks.length) return `<p class="empty">${tr("暂无打点")}</p>`;
    if (kind === "review") return marks.map(mark => `<div class="chapter-row review-chapter" data-mark-time="${mark.at}">
        <button type="button" class="chapter-jump" data-action="review-seek" data-value="${mark.at}" title="跳转至 ${time(mark.at)}">
          <time>${time(mark.at)}</time></button>
        <button type="button" class="chapter-name" data-action="review-edit-mark" data-value="${mark.at}"
          title="${esc(mark.name)}" aria-label="编辑打点 ${esc(mark.name)}"><span class="truncate">${esc(mark.name)}</span>${g("pen")}</button>
      </div>`).join("");
    return marks.map(mark => `<button type="button" class="chapter-row" data-action="${kind}-seek" data-value="${mark.at}">
      <time>${time(mark.at)}</time><span class="truncate" title="${esc(mark.name)}">${esc(mark.name)}</span></button>`).join("");
  }

  function mediaPlayer(kind, asset) {
    if (!asset?.src) return `<span class="empty-preview soft">${tr("请选择视频")}</span>`;
    return `<video id="${kind}-video" data-asset-id="${esc(asset.id)}" playsinline crossorigin="anonymous" preload="auto"
      src="${esc(asset.playback || asset.src)}"></video>`;
  }

  function videoInformation(asset) {
    const frameRate = Number(asset.frameRate);
    const entries = [
      ["编码", asset.codec], ["分辨率", asset.width && asset.height ? asset.width + "×" + asset.height : ""],
      ["帧率", asset.id && Number.isFinite(frameRate) && frameRate > 0 ? Number(frameRate.toFixed(2)) + " fps" : ""],
      ["码率", asset.bitrate ? Math.round(asset.bitrate) + " kbps" : ""], ["帧数", asset.frames],
      ["音轨", asset.audioTracks], ["体积", asset.bytes ? (asset.bytes / 1048576).toFixed(1) + " MB" : ""],
    ];
    return entries.map(([label, value]) => `<div><dt>${tr(label)}</dt><dd>${esc(value ?? "") || "—"}</dd></div>`).join("");
  }

  function reviewPage() {
    const asset = currentAsset();
    return `<div class="page review-page${state.review.collapsed ? " library-collapsed" : ""}">
      <aside class="panel library">
        ${head("素材库", "folder", `<div class="hstack">${ib("open-local", "打开本地视频", "plus", { variant: "flat mini open-local" })}
        ${ib("library-collapse", state.review.collapsed ? "展开素材库" : "收起素材库", "collapse")}</div>`)}
        <div class="top-tools stack">
          <label class="search-box">${g("search")}<input id="library-search" type="search" aria-label="${tr("搜索视频名")}" placeholder="${tr("搜索视频名")}" value="${esc(state.review.query)}"></label>
          ${tabs("library-filter", [["all", "全部"], ["normal", "正式"], ["recoverable", "可恢复"]], state.review.filter, "素材类型")}
        </div><div class="scroll-body library-list">${libraryItems()}</div>
      </aside>
      <section class="panel review-player" data-screen="review">
        <div class="panel-head"><div class="monitor-title grow"><h1 class="truncate" title="${esc(asset.name || asset.title)}">${esc(asset.title)}</h1></div>
          <div class="hstack">${assistantToggle()}
          ${b("enter-editor", "进入剪辑", "scissors", { variant: "primary" })}
          ${b("export", "导出", "download", { variant: "toolbar-export", disabled: !asset.id })}${leaveFull()}</div>
        </div>
        <div class="media-surface"><div class="media-fit"><div class="media-frame">
          ${mediaPlayer("review", asset)}
          <button type="button" class="center-play" id="center-play" data-action="review-play" title="播放" aria-label="播放"
            ${state.review.playing || state.review.at !== 0 ? "hidden" : ""}>${g("play")}</button>
        </div></div></div>
        ${playerControls("review", state.review.at, asset.duration, asset.marks)}
      </section>
      <aside class="panel review-info">
        ${head("打点分节", "flag", ib("review-mark", "在当前进度打点", "plus"), `<span id="review-mark-count">(${asset.marks.length})</span>`)}
        <div class="scroll-body chapter-list">${chapters(asset.marks, "review")}</div>
        <section class="encode-data">${head("视频信息", "chip")}<dl>${videoInformation(asset)}</dl></section>
      </aside>
    </div>`;
  }

  function editorBin() {
    let body;
    if (state.editor.bin === "chapters") body = `<div class="chapter-list">${chapters(timeline.markers, "editor")}</div>`;
    else if (state.editor.bin === "stills") body = `<div class="stack">
      <div class="hstack still-actions">
        ${b("still-add", "保存当前", "camera", { variant: "mini" })}
        ${b("still-import", "导入图片", "plus", { variant: "mini" })}
        <input type="file" id="editor-still-import" accept="image/png,image/jpeg,image/webp,image/avif,image/bmp,image/gif" multiple hidden>
      </div>${editorStills().map(item => `<div class="still-row">
        <button type="button" class="media-item" data-action="still-preview" data-value="${esc(item.id)}"
          title="${esc(item.title)}"${item.image ? "" : " disabled"}>
          <div class="media-thumb">${item.image ? `<img src="${esc(item.image)}" alt="">` : `<span class="source-icon">${g("image")}</span>`}</div>
          <div class="media-summary"><strong class="truncate">${esc(item.title)}</strong>${item.at != null ? `<small>${time(item.at)}</small>` : ""}</div>
        </button>
        ${ib("still-insert", "插入到播放头", "plus", { value: item.id, disabled: !item.image })}
      </div>`).join("")}</div>`;
    else body = `<small>${tr("工程素材")}</small><div class="bin-media">${[editorAsset(), ...assets.filter(asset =>
      asset.id !== state.editorAsset && asset.kind === "normal" && !asset.local)].map((asset, i) =>
      mediaItem(asset, "editor-media", i === 0, false)).join("")}</div>
      <hr class="subdivider">${b("open-local", "导入外部视频", "plus", { variant: "flat" })}
      ${b("export", "导出选中片段", "download", { value: "clip", variant: "flat" })}`;
    return `<aside class="panel editor-bin">${head("素材工程", "folder", ib("save", "保存工程", "save"))}
      <div class="inset stack scroll-body project-tools">
        ${tabs("bin-tab", [["media", "素材"], ["stills", "静止画面"], ["chapters", "章节"]], state.editor.bin, "工程素材分组")}
        ${body}</div></aside>`;
  }

  function subtitlePanel() {
    const subTracks = timeline.tracks.filter(track => track.kind === "subtitle");
    const track = subTracks.find(track => track.id === state.editor.subtitleTrack) ?? subTracks[0];
    const audioTracks = timeline.tracks.filter(track => track.kind === "audio");
    return `
      <div class="form-row tight">
        ${select("subtitle-track", "字幕轨", subTracks.map(t => [t.id, t.id + " · " + t.name]), track?.id)}
        ${ib("subtitle-style", "字幕轨样式", "cog", { variant: "mini", value: "track:" + (track?.id || "") })}
        ${ib("add-line", "添加字幕句", "plus", { variant: "mini" })}
        ${ib("srt-export", "导出 SRT", "download", { variant: "mini" })}
        ${ib("srt-import", "导入 SRT", "folder", { variant: "mini" })}
      </div>
      <section class="field-group">
        <div class="hstack spaced"><strong class="hstack">${g("mic")} ${tr("本地识别与中英互译")}</strong>
          ${b("proofread", "AI 校对", "spark", { variant: "mini" })}</div>
        <div class="form-row tight">
          ${select("asr-track", "识别音轨", audioTracks.map(t => [t.id, t.id + " " + t.name]), state.editor.asrTrack)}
          ${select("asr-lang", "识别语言", ["简体中文", "英语"], state.editor.asrLanguage)}
          ${b("recognize", "识别", null, { variant: "mini primary" })}
        </div>
        <div class="form-row tight">${select("translation-direction", "翻译方向", ["中 → 英", "英 → 中"], state.editor.direction)}
        <input type="text" data-field="glossary" aria-label="${tr("术语表")}" placeholder="${tr("术语表")}" value="${esc(state.editor.glossary)}">
        ${b("translate", "翻译", null, { variant: "mini" })}</div>
      </section>
      <label class="subtitle-search">
        <input type="search" placeholder="${tr("搜索原文或译文")}" data-field="subtitle-search" aria-label="${tr("搜索字幕")}" value="${esc(state.editor.subtitleQuery)}">
      </label>
      <div id="subtitle-list" class="stack">${subtitleRows(track)}</div>`;
  }

  function subtitleRows(track) {
    track ??= timeline.tracks.find(item => item.id === state.editor.subtitleTrack);
    const q = state.editor.subtitleQuery.toLowerCase().trim();
    return (track?.clips ?? []).filter(clip => `${clip.name} ${clip.translation || ""}`.toLowerCase().includes(q))
      .slice().sort((a, b) => a.at - b.at).map(clip => `
        <div class="subtitle-card"><div class="hstack spaced">
          <button type="button" class="subtitle-time" data-action="editor-seek" data-value="${clip.at}">
            <time>${time(clip.at)} → ${time(clip.at + clip.length)}</time></button>
          <div class="hstack">
            ${ib("subtitle-style", "字幕片段样式", "cog", { variant: "flat mini", value: "clip:" + clip.id })}
            ${b("retranslate", "重译", null, { variant: "flat mini", value: clip.id })}
            ${ib("remove-line", "删除字幕", "trash", { value: clip.id })}</div></div>
          <input type="text" data-field="subtitle-original" data-clip-id="${clip.id}" value="${esc(clip.name)}" aria-label="字幕原文">
          <input type="text" class="translation" data-field="subtitle-translation" data-clip-id="${clip.id}" value="${esc(clip.translation)}" aria-label="字幕译文">
        </div>`).join("") || `<p class="empty">${tr("暂无字幕")}</p>`;
  }

  function dubbingPanel() {
    const dub = state.editor.dub;
    const modes = [["sentences", "字幕逐句配音"], ["custom", "自填旁白"], ["chapter", "章节长稿"]];
    const track = timeline.tracks.find(item => item.id === state.editor.subtitleTrack);
    const services = state.ttsServices.map(({id, label}) => [id, label]);
    const engine = services.find(([id]) => id === dub.engine)?.[0] || services[0]?.[0];
    const placement = dub.mode === "sentences" ? tr("按字幕逐句生成") :
      dub.mode === "chapter" ? tr("按章节起点编排") :
        tr("从播放头 %1 开始").replace("%1", `<time id="dub-start">${time(timeline.at)}</time>`);
    return `
      ${tabs("dub-mode", modes, dub.mode, "配音模式")}
      <section class="field-group">
        <div class="hstack spaced">${tabs("dub-service", [["local", "本地配音服务"], ["online", "在线配音服务"]], dub.service, "配音服务")}
        <span class="tag ${dub.service === "local" ? "waiting" : ""}">${tr(dub.service === "local" ? "离线" : "在线")}</span></div>
        <div class="form-row">
        ${dub.service === "local" ? select("voice", "配音音色", ["普通话女声", "普通话男声"], dub.voice) :
          engine ? select("dub-engine", "在线配音服务", services, engine) :
            b("native-models", "配置在线配音", "cog", { variant: "mini", value: "tts" })}
        ${b("audition", "试听音色", "play", { variant: "mini", disabled: dub.service === "online" && !engine })}</div>
        <label class="hstack"><small>${tr("语速")}</small><input class="grow" type="range" min="0.5" max="2" step=".05" data-range="dub-speed"
          value="${dub.speed}"><output class="mono" id="dub-speed">${dub.speed.toFixed(2)}×</output></label>
        <label class="hstack"><small>${tr("音量")}</small><input class="grow" type="range" min="0" max="100" data-range="dub-volume"
          value="${dub.volume}"><output class="mono" id="dub-volume">${dub.volume}%</output></label>
      </section>
      <div class="hstack spaced"><small>${placement}</small>
        <div class="hstack">${b("dub-generate", dub.mode === "custom" ? "生成旁白" : "批量生成", null, { variant: "mini primary" })}
        ${b("dub-add", "加入独立音轨", "plus", { variant: "mini" })}</div></div>
      ${dub.mode === "sentences" ? (track?.clips ?? []).map((clip, i) => `
      <div class="subtitle-card">
        <div class="hstack spaced">${select("dub-text-" + i, "配音文本选择", ["使用原文", "使用译文", "独立配音稿"], dub.textType[i] || "使用原文")}
          <time>${time(clip.at)} → ${time(clip.at + clip.length)}</time></div>
        <input type="text" aria-label="配音稿" data-field="dub-script-${i}" value="${esc(dub.scripts[i] ??
          (dub.textType[i] === "使用译文" ? clip.translation : clip.name))}">
        <input type="text" aria-label="${tr("读音规则")}" placeholder="${tr("读音规则（多音字、缩写、重音）")}" data-field="dub-rule-${i}" value="${esc(dub.rules[i])}">
        <div class="hstack">${b("dub-original", "原声", null, { variant: "mini", value: clip.at })}
          ${b("audition", "试听", "play", { variant: "mini", value: i })}${b("dub-generate", "重做", "refresh", { variant: "mini", value: i })}</div>
      </div>`).join("") : dub.mode === "chapter" ? `
        <div class="hstack spaced">${b("chapter-script-import", "导入稿件…", "folder", { variant: "mini" })}
        ${checkbox("dub-duck", "配音时压低原声", dub.duck)}
        </div><small>${tr("纯文本导入当前章节。多章使用「## 章节标题」。")}</small>
        ${chapterNarrationSections().map(section => `<div class="subtitle-card">
          <div class="hstack spaced"><strong class="truncate">${esc(section.title)}</strong>
            <time>${time(section.at)} → ${time(section.at + section.length)}</time></div>
          <textarea rows="4" maxlength="60000" data-field="dub-chapter-${section.uid}" placeholder="${tr("输入章节稿件")}"
            aria-label="${esc(section.title)}配音稿">${esc(dub.chapterScripts[section.uid])}</textarea>
          <input type="text" maxlength="1200" data-field="dub-chapterrule-${section.uid}" value="${esc(dub.chapterRules[section.uid])}"
            placeholder="${tr("读音规则（原文=读音）")}" aria-label="${esc(section.title)}读音规则">
          <div class="hstack">${b("audition", "试听", "play", { variant: "mini", value: section.uid })}</div>
        </div>`).join("")}` : `
      <textarea rows="4" maxlength="4000" data-field="dub-narration" placeholder="${tr("输入旁白内容")}" aria-label="${tr("旁白内容")}">${esc(dub.narration)}</textarea>
      <input type="text" data-field="dub-narrationRule" value="${esc(dub.narrationRule)}"
        placeholder="${tr("读音规则（原文=读音）")}" aria-label="${tr("旁白读音规则")}">`}`;
  }

  function editorPage() {
    const crop = state.editor.crop;
    const layout = state.editorLayout;
    return `<div class="page editor-page" style="--editor-left:${layout.left}px;--editor-right:${layout.right}px;--editor-timeline:${layout.timeline}%"><div class="editor-upper">
      ${editorBin()}
      ${splitHandle("editor-left", "调整素材区宽度")}
      <section class="panel program-monitor" data-screen="editor">
        ${head("成片预览", "monitor", `<div class="hstack">
          ${assistantToggle()}${b("crop-toggle", "裁剪取景", "crop", { variant: "mini", pressed: !!crop, disabled: !editorAsset().id })}${leaveFull()}</div>`)}
        <div class="media-surface">
          ${crop ? `<div class="crop-tools">${tabs("crop-ratio", ["自由", "16:9", "9:16", "1:1", "自定义"].map(item =>
            [item, item === "自定义" && crop === item ? `${state.editor.cropRatio.width}:${state.editor.cropRatio.height}` : item]),
            crop, "取景比例")}</div>` : ""}
          <div class="media-fit"><div class="media-frame">${mediaPlayer("editor", editorAsset())}
            <img id="editor-preview-still" alt="" hidden>
            ${crop && editorAsset().id ? `<div class="crop-outline" style="${editorCropGeometry()}">
              ${["nw", "ne", "sw", "se"].map(corner => `<span data-crop-corner="${corner}"></span>`).join("")}</div>` : ""}
            <div id="editor-subtitles" class="subtitle-overlay"></div>
          </div></div>
          ${ib("still-close", "关闭图片预览", "close", { variant: "still-preview-close mini" })}
        </div>${playerControls("editor", timeline.at, timeline.duration, timeline.markers)}
      </section>
      ${splitHandle("editor-right", "调整编辑区宽度")}
      <aside class="panel editor-options">${editorOptionTabs()}<div class="inset stack scroll-body">${editorOptionBody()}</div></aside>
    </div>${splitHandle("editor-bottom", "调整时间轴高度", true)}
      <section class="panel timeline" id="timeline" aria-label="剪辑时间轴"></section></div>`;
  }

  function editorOptionTabs() {
    return `<div class="panel-head">${tabs("editor-tab", [["subtitle", "字幕与校对", "text"], ["dub", "配音与旁白", "mic"]],
      state.editor.tab, "剪辑工具")}</div>`;
  }

  function editorOptionBody() {
    return state.editor.tab === "subtitle" ? subtitlePanel() : dubbingPanel();
  }

  function aiPage() {
    return AgentWorkspace.page();
  }

  function assistant() {
    return AgentWorkspace.sidebar();
  }

  function floating() {
    if (!state.floating) return "";
    if (state.floatSmall) return `<div class="floating-bar floating-small" aria-label="悬浮控制条">
      <button type="button" class="btn flat icon-btn" data-action="float-expand" data-floating-grip title="展开或拖动悬浮条" aria-label="展开悬浮条">
       <img src="assets/app.svg" alt=""></button></div>`;
    const phase = state.record.phase;
    const active = phase !== "idle";
    return `<div class="floating-bar hstack" aria-label="悬浮控制条">
      <button type="button" class="btn flat mini icon-btn floating-grip" data-floating-grip
        title="拖动悬浮条" aria-label="移动悬浮条">${g("grip")}</button>
      <i class="dot ${active ? "live" : "green"}"></i><time class="mono" data-clock>${time(state.record.seconds, true)}</time>
      ${ib("record-action", phase === "countdown" ? "取消倒计时" : active ? "停止录制" : "开始录制",
        active ? "stop" : "play", { variant: "mini floating-record", pressed: active })}
      ${ib("screenshot", "截图", "camera")}${ib("record-mark", "打点", "flag",
      { disabled: !["recording", "paused"].includes(state.record.phase) })}<i class="thin-rule"></i>
      ${ib("floating-workbench", "打开录制工作台", "monitor")}
      ${ib("floating-annotate", "讲解工具", "pen", { pressed: state.annotation.open })}<i class="thin-rule"></i>
      ${ib("float-expand", "折叠悬浮条", "right")}${ib("float-toggle", "关闭悬浮条", "close")}
    </div>`;
  }

  return { titlebar, status, recordPage, mixerDock, controlDock, reviewPage, libraryItems, seekMarkers, chapters, editorPage, subtitleRows, aiPage, assistant, annotationTools, videoInformation,
    floating, currentAsset, editorAsset, formSelect, checkbox, outputPath, editorBin, editorOptionTabs, editorOptionBody, seeker };
})();
