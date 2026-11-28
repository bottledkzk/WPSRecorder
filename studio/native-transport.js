"use strict";

const Native = (() => {
  const pending = new Map();
  const listeners = new Map();
  const transport = globalThis.chrome?.webview;
  let sequence = 0;
  transport?.addEventListener("message", ({ data }) => {
    if (data?.event) {
      for (const listener of listeners.get(data.event) ?? []) listener(data.value);
      return;
    }
    const item = pending.get(data?.id);
    if (!item) return;
    pending.delete(data.id);
    if (data.error) item.reject(new Error(data.error));
    else item.resolve(data.result);
  });
  return {
    rpc(command, data = {}) {
      if (!transport) return Promise.reject(new Error("请从 WPS Recorder 启动 Studio。"));
      const id = String(++sequence);
      const message = { id, command, data };
      return new Promise((resolve, reject) => {
        if (JSON.stringify(message).length > 2 * 1024 * 1024) {
          reject(new Error("请求内容过大，请拆分后重试。"));
          return;
        }
        pending.set(id, { resolve, reject });
        try {
          transport.postMessage(message);
        } catch (error) {
          pending.delete(id);
          reject(error);
        }
      });
    },
    tell(command, data = {}) { transport?.postMessage({ command, data }); },
    on(event, listener) {
      if (!listeners.has(event)) listeners.set(event, []);
      listeners.get(event).push(listener);
    },
  };
})();
