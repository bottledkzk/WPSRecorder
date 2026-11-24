"use strict";

const ConversationCompact = (() => {
  const recentMessages = 6;
  const automaticMessages = 16;
  const automaticCharacters = 8800;
  const maxSummary = 1150;
  const continuation = /^(继续|接着做|按刚才的来)[。！!]?$/u;
  const active = new Map();

  function boundary(thread) {
    const index = Number(thread.compaction?.through);
    return Number.isInteger(index) && index >= 0 && index <= thread.messages.length ? index : 0;
  }

  function context(thread) {
    const through = boundary(thread);
    return {
      summary: through ? thread.compaction.summary : "",
      recent: thread.messages.slice(through),
      lastRequest: through ? thread.compaction.lastRequest : "",
    };
  }

  function compactPlan(thread, force) {
    const from = boundary(thread);
    const remaining = thread.messages.slice(from);
    const size = remaining.reduce((sum, entry) => sum + String(entry.text ?? "").length, 0);
    if (!force && remaining.length < automaticMessages && size < automaticCharacters) return null;
    const retained = force ? Math.min(recentMessages, Math.max(2, remaining.length - 2)) : recentMessages;
    let through = thread.messages.length - retained;
    while (through > from && thread.messages[through]?.who === "assistant") through--;
    if (through <= from) return null;
    const messages = thread.messages.slice(from, through);
    const previous = from ? thread.compaction.summary : "";
    return { messages, through, previous };
  }

  function eligible(thread) {
    return !active.has(thread) && !!compactPlan(thread, true);
  }

  function brief(text, limit) {
    const line = String(text ?? "").replace(/\s+/g, " ").trim();
    return line.length > limit ? line.slice(0, limit - 1) + "…" : line;
  }

  function localSummary(plan) {
    const recent = plan.messages.slice(-6).map(message =>
      (message.who === "user" ? "要求 · " : "进展 · ") + brief(message.text, 135));
    return [brief(plan.previous, 275), ...recent].filter(Boolean).join("\n").slice(0, maxSummary);
  }

  async function modelSummary(plan, job) {
    const history = plan.messages.map(message => ({
      role: message.who === "user" ? "user" : "assistant", content: String(message.text ?? "").slice(0, 3000),
    }));
    const summary = await Native.rpc("compact", { job, messages: [{
      role: "system",
      content: "把 WPS Recorder 对话压缩成中文上下文摘要。保留用户目标、决定、项目、素材名称、时间点、参数和未完成事项。仅输出摘要。",
    }, { role: "user", content: JSON.stringify({ previousSummary: plan.previous, olderMessages: history }) }] });
    if (typeof summary !== "string" || !summary.trim()) throw new Error("模型没有返回摘要");
    return summary.trim().slice(0, maxSummary);
  }

  async function run(thread, { force = false, update }) {
    if (active.has(thread)) return false;
    const plan = compactPlan(thread, force);
    if (!plan) return false;
    const controller = new AbortController();
    const job = crypto.randomUUID();
    controller.signal.addEventListener("abort", () => Native.tell("assistant-cancel", { job }), { once: true });
    active.set(thread, controller);
    update();
    let method = "local";
    let summary;
    try {
      summary = await modelSummary(plan, job);
      method = "model";
    } catch {
      // The local summary keeps the conversation usable if the model cannot be reached.
    } finally {
      if (active.get(thread) === controller) active.delete(thread);
    }
    if (controller.signal.aborted) return false;
    const lastRequest = plan.messages.findLast(entry => entry.who === "user" && !continuation.test(entry.text))?.text
      || context(thread).lastRequest;
    thread.compaction = { through: plan.through, summary: summary || localSummary(plan), method, lastRequest };
    update();
    return true;
  }

  function continueRequest(thread, question) {
    if (!continuation.test(question)) return question;
    const saved = context(thread);
    const previous = saved.recent.findLast(entry => entry.who === "user" && !continuation.test(entry.text));
    return previous?.text || saved.summary || saved.lastRequest || question;
  }

  function status(thread) {
    const running = active.has(thread);
    const compacted = boundary(thread) ? thread.compaction : null;
    if (!running && !compacted) return "";
    if (running) return '<div class="agent-context-state" role="status"><i class="agent-spinner" aria-hidden="true"></i>正在压缩上下文</div>';
    const label = compacted.method === "model" ? "模型摘要" : "本地摘要";
    return `<details class="agent-context-state"><summary title="${label}">${UI.glyph("layers")} ${label} · 已压缩 ${compacted.through} 条</summary>
      <p>${UI.escape(compacted.summary)}</p></details>`;
  }

  function openModelSettings() { settingsDialog("models"); }
  function modelName() { return "当前助手模型"; }
  function clearModel() {}

  function reset() {
    for (const controller of active.values()) controller.abort();
    active.clear();
  }

  function cancel(thread) {
    active.get(thread)?.abort();
    active.delete(thread);
  }

  return { run, eligible, context, continueRequest, status, openModelSettings, clearModel, reset, cancel, modelName };
})();
