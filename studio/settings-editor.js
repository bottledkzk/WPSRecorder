"use strict";

const SettingsEditors = (() => {
  const roles = [["assistant", "录屏助手"], ["translate", "字幕翻译"], ["video", "视频理解"], ["tts", "语音合成"]];
  let dispose = null;

  const text = label => UI.escape(UI.text(label));
  const byName = (form, name) => form.elements.namedItem(name);
  const action = (label, attrs = "", primary = false) =>
    '<button type="button" class="btn mini' + (primary ? ' primary' : '') + '" ' + attrs + '>' + text(label) + '</button>';
  const feedback = '<small class="settings-feedback" role="status" aria-live="polite"></small>';
  const namedKeys = { ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down", Escape: "Esc",
    Insert: "Ins", Delete: "Del", Backspace: "Backspace", Home: "Home", End: "End", PageUp: "PgUp", PageDown: "PgDown",
    Tab: "Tab", Enter: "Return", Space: "Space", PrintScreen: "Print", Pause: "Pause", Minus: "-", Equal: "=",
    BracketLeft: "[", BracketRight: "]", Backslash: "\\", Semicolon: ";", Quote: "'", Backquote: "\x60",
    Comma: ",", Period: ".", Slash: "/" };

  function showStatus(container, message, error = false) {
    const status = container.querySelector(".settings-feedback");
    if (!status) return;
    status.textContent = UI.text(message);
    status.classList.toggle("error", error);
  }

  function leave() {
    dispose?.();
    dispose = null;
  }

  function hotkeys(container) {
    const fields = [];
    container.innerHTML = '<div class="settings-key-grid"></div><div class="hstack settings-edit-actions">' +
      action("全部清空", 'data-key-clear-all') + action("保存快捷键", 'data-key-save', true) + '</div>' + feedback;
    const grid = container.querySelector(".settings-key-grid");
    const save = container.querySelector("[data-key-save]");
    let capturing = false;
    const capture = active => {
      if (capturing === active) return;
      capturing = active;
      Native.tell("hotkeys-edit", { active });
    };
    const stopCapture = () => capture(false);
    const resumeCapture = () => { if (grid.contains(document.activeElement) && document.activeElement.matches("input")) capture(true); };
    window.addEventListener("blur", stopCapture);
    window.addEventListener("focus", resumeCapture);
    dispose = () => {
      window.removeEventListener("blur", stopCapture);
      window.removeEventListener("focus", resumeCapture);
      stopCapture();
    };
    (state.hotkeyLabels || []).forEach((name, index) => {
      const label = document.createElement("label");
      label.htmlFor = "system-hotkey-" + index;
      label.textContent = UI.text(name);
      const field = document.createElement("input");
      field.id = label.htmlFor;
      field.type = "text";
      field.readOnly = true;
      field.className = "mono";
      field.placeholder = UI.text("未设置");
      field.title = UI.text("聚焦后按下快捷键");
      field.value = state.hotkeys[index] || "";
      const clear = document.createElement("button");
      clear.type = "button";
      clear.className = "btn mini icon-btn";
      clear.title = UI.text("清除");
      clear.setAttribute("aria-label", UI.text("清除热键：%1").replace("%1", name));
      clear.innerHTML = UI.glyph("close");
      const set = keys => { field.dataset.portable = keys; field.value = keys.replace("Meta+", "Win+"); clear.disabled = !keys; };
      set(field.value);
      clear.addEventListener("click", () => set(""));
      field.addEventListener("focus", () => { capture(true); field.select(); });
      field.addEventListener("blur", () => capture(false));
      field.addEventListener("keydown", event => {
        const modified = event.ctrlKey || event.altKey || event.metaKey;
        if (["Control", "Alt", "Shift", "Meta", "AltGraph"].includes(event.key) ||
            (!modified && ["Tab", "Escape"].includes(event.key))) return;
        event.preventDefault();
        event.stopPropagation();
        if (!modified && ["Backspace", "Delete"].includes(event.key)) {
          set("");
          return;
        }
        const key = /^Key[A-Z]$/.test(event.code) ? event.code.slice(3) :
          /^Digit[0-9]$/.test(event.code) ? event.code.slice(5) :
          /^F(?:[1-9]|1[0-9]|2[0-4])$/.test(event.key) ? event.key : (namedKeys[event.code] || namedKeys[event.key]);
        if (!key || !modified) {
          showStatus(container, "请使用含 Ctrl、Alt 或 Win 的单个快捷键组合。", true);
          return;
        }
        set([event.ctrlKey && "Ctrl", event.altKey && "Alt", event.shiftKey && "Shift", event.metaKey && "Meta", key].filter(Boolean).join("+"));
        showStatus(container, "");
      });
      fields.push({ field, set });
      grid.append(label, field, clear);
    });
    container.querySelector("[data-key-clear-all]").addEventListener("click", () => {
      fields.forEach(({set}) => set(""));
      showStatus(container, "");
    });
    save.addEventListener("click", async () => {
      save.disabled = true;
      capture(false);
      try {
        state.hotkeys = await Native.rpc("hotkeys-save", { keys: fields.map(({field}) => field.dataset.portable) });
        if (container.isConnected) {
          fields.forEach(({set}, i) => set(state.hotkeys[i] || ""));
          showStatus(container, "快捷键已保存。");
        }
      } catch (error) { if (container.isConnected) showStatus(container, error.message, true); }
      finally { save.disabled = false; }
    });
  }

  function modelProtocols(role) {
    const common = [["", "选择接口协议"], ["openai", "OpenAI 兼容"]];
    const extra = role === "tts" ? [["dashscope", "阿里云 DashScope TTS"], ["minimax", "MiniMax TTS 原生"]] :
      role === "video" ? [["anthropic", "Anthropic Messages"]] :
      [["anthropic", "Anthropic Messages"], ["ollama", "Ollama 原生对话"]];
    return [...common, ...extra, ["json", "自定义 JSON HTTP"]];
  }

  function modelSelect(label, name, choices, value) {
    return '<label for="studio-model-' + name + '">' + text(label) + '</label><select id="studio-model-' + name +
      '" name="' + name + '">' + choices.map(([key, caption]) => '<option value="' + UI.escape(key) + '"' +
        (key === value ? ' selected' : '') + '>' + text(caption) + '</option>').join("") + '</select>';
  }

  function modelInput(label, name, value, attrs = "") {
    return '<label for="studio-model-' + name + '">' + text(label) + '</label><input id="studio-model-' + name +
      '" type="' + (name === "key" ? "password" : "text") + '" name="' + name + '" value="' + UI.escape(value ?? "") + '" ' + attrs + '>';
  }

  function modelForm(profile, role) {
    const tts = role === "tts";
    return '<form id="studio-model-form" class="stack" autocomplete="off">' +
      '<fieldset class="settings-model-main form">' +
      (tts ? modelInput("配置名称", "label", profile.label, 'maxlength="80"') : '') +
      modelSelect("接口协议", "protocol", modelProtocols(role), profile.protocol) +
      modelInput("接口地址", "endpoint", profile.endpoint, 'maxlength="2048" spellcheck="false" required') +
      modelInput("模型", "model", profile.model, 'maxlength="250" spellcheck="false" required') +
      (tts ? modelInput("默认音色 ID", "voice", profile.voice, 'spellcheck="false"') : '') +
      modelInput("API 密钥", "key", "", 'autocomplete="new-password" spellcheck="false"') + '</fieldset>' +
      '<details class="settings-service-auth"><summary>' + text("认证选项") + '</summary><div class="form">' +
      modelInput("请求头名称", "credentialHeader", profile.credentialHeader ?? "Authorization", 'maxlength="64" spellcheck="false"') +
      modelInput("密钥前缀", "credentialPrefix", profile.credentialPrefix ?? "Bearer ", 'maxlength="100" spellcheck="false"') +
      '</div>' + action("移除此地址的已存密钥", 'data-model-clear disabled') + '</details>' +
      '<fieldset class="settings-model-custom form" hidden>' +
      '<label for="studio-model-requestTemplate">' + text("请求 JSON") + '</label><textarea id="studio-model-requestTemplate" ' +
      'name="requestTemplate" rows="3" maxlength="16000" spellcheck="false">' + UI.escape(profile.requestTemplate ?? "") + '</textarea>' +
      (tts ? modelSelect("音频结果", "audioEncoding", [["binary", "音频二进制"], ["base64", "JSON 中的 Base64"],
        ["hex", "JSON 中的十六进制"]], profile.audioEncoding ?? "binary") : '') +
      modelInput("响应字段", "responsePath", profile.responsePath, 'spellcheck="false"') + '</fieldset>' +
      '<div class="hstack settings-edit-actions">' + action("检验能力", 'data-model-check') +
      '<button type="submit" class="btn mini primary">' + text("保存当前配置") + '</button></div></form>';
  }

  function models(container, initial) {
    let role = roles.some(([id]) => id === initial && id !== "tts") ? initial : initial ? "tts" : "assistant";
    let selected = role === "tts" && initial !== "tts" ? initial : "";
    let profiles = [];
    let checking = null;
    let active = true;
    const drafts = new Map();
    dispose = () => {
      active = false;
      if (checking) Native.tell("models-cancel");
      const key = container.querySelector('input[type="password"]');
      if (key) key.value = "";
    };
    function receive(data) {
      profiles = data.profiles || [];
      const changed = JSON.stringify(state.ttsServices) !== JSON.stringify(data.tts);
      state.ttsServices = data.tts || [];
      if (changed) render();
    }
    function remember() {
      const form = container.querySelector("form");
      if (!form) return;
      const values = Object.fromEntries(new FormData(form));
      delete values.key;
      drafts.set(form.dataset.profile, { ...profiles.find(row => row.id === form.dataset.profile), ...values });
    }
    function paint(message = "") {
      if (!active || !container.isConnected) return;
      const tts = role === "tts";
      const choices = profiles.filter(row => !roles.some(([id]) => id === row.id));
      for (const [id, draft] of drafts)
        if (id.startsWith("tts-") && !choices.some(row => row.id === id)) choices.push({id, ...draft});
      if (tts && !choices.some(row => row.id === selected)) selected = choices[0]?.id || "";
      const id = tts ? selected : role;
      const profile = drafts.get(id) || profiles.find(row => row.id === id);
      const header = '<div class="form settings-model-header">' + modelSelect("服务", "role", roles, role) +
        (tts ? '<label for="studio-model-profile">' + text("配音配置") + '</label><div class="hstack settings-profile-actions">' +
          '<select id="studio-model-profile" aria-label="' + text("配音配置") + '">' +
          (choices.length ? choices.map(row => '<option value="' + UI.escape(row.id) + '"' + (row.id === selected ? ' selected' : '') +
            '>' + UI.escape(row.label || row.model || UI.text("新配置（未保存）")) + '</option>').join("") :
            '<option>' + text("暂无配音配置") + '</option>') + '</select>' +
          action("新增配置", 'data-profile-add') + action("删除配置", 'data-profile-remove' + (profile ? '' : ' disabled')) + '</div>' : '') +
        '</div>';
      container.innerHTML = header + (profile ? modelForm(profile, role) : "") + feedback;
      showStatus(container, message);
      container.querySelector("#studio-model-role").addEventListener("change", event => {
        remember();
        role = event.target.value;
        paint();
        container.querySelector("#studio-model-role").focus();
      });
      if (tts) {
        container.querySelector("#studio-model-profile").addEventListener("change", event => {
          remember();
          selected = event.target.value;
          paint();
          container.querySelector("#studio-model-profile").focus();
        });
        container.querySelector("[data-profile-add]").addEventListener("click", () => {
          remember();
          selected = "tts-" + crypto.randomUUID().replaceAll("-", "");
          drafts.set(selected, {});
          paint();
          container.querySelector("#studio-model-label").focus();
        });
        const remove = container.querySelector("[data-profile-remove]");
        remove.addEventListener("click", async () => {
          if (remove.dataset.confirm !== id) {
            remove.dataset.confirm = id;
            remove.textContent = UI.text("确认删除");
            return;
          }
          remove.disabled = true;
          try {
            if (profiles.some(row => row.id === id)) receive(await Native.rpc("models-remove", { id }));
            drafts.delete(id);
            selected = "";
            paint("配音配置已删除。");
          } catch (error) {
            remove.disabled = false;
            if (active) showStatus(container, error.message, true);
          }
        });
      }
      if (!profile) return;
      const form = container.querySelector("form");
      form.dataset.profile = id;
      const protocol = byName(form, "protocol");
      const key = byName(form, "key");
      const endpoint = byName(form, "endpoint");
      const headerInput = byName(form, "credentialHeader");
      const prefix = byName(form, "credentialPrefix");
      const template = byName(form, "requestTemplate");
      const check = form.querySelector("[data-model-check]");
      const clear = form.querySelector("[data-model-clear]");
      const submit = form.querySelector("[type='submit']");
      const saved = profiles.find(row => row.id === id);
      function refreshProtocol() {
        const custom = protocol.value === "json";
        form.querySelector(".settings-model-custom").hidden = !custom;
        if (custom && !template.value.trim()) template.value = tts ?
          '{"model":"$model","text":"$text","voice":"$voice","format":"$format","speed":"$speed"}' :
          '{"model":"$model","messages":"$messages","tools":"$tools","stream":false}';
        endpoint.placeholder = protocol.value === "anthropic" ? "https://api.anthropic.com/v1/messages" :
          protocol.value === "ollama" ? "http://127.0.0.1:11434/api/chat" : UI.text("填写完整的请求 URL");
      }
      function refreshCredential() {
        const retained = saved?.hasKey && endpoint.value.trim() === saved.endpoint;
        key.placeholder = UI.text(retained ? "已配置 · 留空保留" : "如服务需要，请填写密钥");
        clear.disabled = !retained;
      }
      function updateProtocol() {
        if (protocol.value === "anthropic" && headerInput.value === "Authorization" && prefix.value === "Bearer ") {
          headerInput.value = "x-api-key";
          prefix.value = "";
        } else if (protocol.value !== "anthropic" && headerInput.value === "x-api-key" && !prefix.value) {
          headerInput.value = "Authorization";
          prefix.value = "Bearer ";
        }
        refreshProtocol();
      }
      protocol.addEventListener("change", updateProtocol);
      endpoint.addEventListener("input", refreshCredential);
      refreshProtocol();
      refreshCredential();
      async function save(clearKey = false) {
        if (!form.reportValidity()) return;
        submit.disabled = true;
        try {
          receive(await Native.rpc("models-save", { id, service: Object.fromEntries(new FormData(form)), clearKey }));
          drafts.delete(id);
          if (form.isConnected && active) paint(clearKey ? "已移除此地址保存的密钥。" : "当前配置已保存。");
        } catch (error) {
          if (form.isConnected && active) showStatus(container, error.message, true);
        } finally { submit.disabled = false; }
      }
      form.addEventListener("submit", event => { event.preventDefault(); save(); });
      clear.addEventListener("click", () => save(true));
      check.addEventListener("click", async () => {
        if (checking) {
          checking.cancelled = true;
          Native.tell("models-cancel");
          check.disabled = true;
          showStatus(container, "正在取消…");
          return;
        }
        if (!form.reportValidity()) return;
        const operation = {cancelled: false};
        checking = operation;
        const controls = [...container.querySelectorAll("input, select, textarea, button")];
        const enabled = controls.filter(control => !control.disabled && control !== check);
        const service = Object.fromEntries(new FormData(form));
        enabled.forEach(control => { control.disabled = true; });
        check.textContent = UI.text("取消检验");
        showStatus(container, "正在发送测试数据并检验服务能力…");
        try {
          await Native.rpc("models-check", { id, service });
          if (active && !operation.cancelled) showStatus(container, tts ? "通过：服务返回的配音可解码播放。" :
            ({assistant: "通过：模型能调用工具并读取工具结果。", translate: "通过：模型返回了可用的中英双向译文。",
              video: "通过：模型识别了视觉样本并返回带时间点的结果。"})[id]);
        } catch (error) {
          if (active && !operation.cancelled) showStatus(container, error.message, true);
        } finally {
          checking = null;
          if (active && operation.cancelled) showStatus(container, "任务已取消。");
          enabled.forEach(control => { control.disabled = false; });
          check.disabled = false;
          check.textContent = UI.text("检验能力");
        }
      });
    }
    container.innerHTML = feedback;
    showStatus(container, "正在读取模型服务…");
    Native.rpc("models-list").then(data => { receive(data); paint(); }, error => {
      if (active && container.isConnected) showStatus(container, error.message, true);
    });
  }

  return { hotkeys, models, leave };
})();
