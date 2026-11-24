"use strict";

class Timeline {
  constructor(change, seek, notify) {
    this.onChange = change;
    this.onSeek = seek;
    this.notify = notify;
    this.reset();
  }

  reset(duration = 152, media = "course", thumbnails = [], frameRate = 24) {
    this.media = media;
    this.thumbnails = thumbnails;
    this.frameRate = frameRate;
    this.markers = [];
    this.track = "V1";
    this.selected = media === "course" ? "v2" : "v1";
    this.at = media === "course" ? 52 : 0;
    this.zoom = 1;
    this.linked = true;
    this.snap = true;
    this.back = [];
    this.forward = [];
    this.trackOrder = [];
    const standard = media === "course";
    this.tracks = [
      { id: "V1", kind: "video", name: "主视频", clips: standard ? [
        { id: "v1", at: 0, length: 40, source: 0 },
        { id: "v2", at: 40, length: 54, source: 40 },
        { id: "v3", at: 94, length: 58, source: 94 },
      ] : [{ id: "v1", at: 0, length: duration, source: 0 }] },
      { id: "A1", kind: "audio", name: "麦克风原声", clips: standard ? [
        { id: "a1", at: 0, length: 40, name: "麦克风原声" },
        { id: "a2", at: 40, length: 54, name: "麦克风原声" },
        { id: "a3", at: 94, length: 58, name: "麦克风原声" },
      ] : [] },
      { id: "A2", kind: "audio", name: "系统声音", clips: standard ? [
        { id: "a4", at: 14, length: 24, name: "系统声音" },
        { id: "a5", at: 54, length: 29, name: "系统声音" },
        { id: "a6", at: 96, length: 42, name: "系统声音" },
      ] : [] },
      { id: "T1", kind: "subtitle", name: "中文字幕", clips: standard ? [
        { id: "t1", at: 4, length: 12, name: "欢迎来到山地摄影课。", translation: "Welcome to the mountain photography course." },
        { id: "t2", at: 19, length: 15, name: "先从山脊的走向开始观察。", translation: "Start by following the ridgeline." },
        { id: "t3", at: 46, length: 11, name: "明暗交界勾勒出山体的轮廓。", translation: "The line between light and shadow defines the ridge." },
        { id: "t4", at: 67, length: 15, name: "把云层放在中景，画面更有层次。", translation: "Place the clouds in the middle ground for depth." },
        { id: "t5", at: 97, length: 16, name: "让视线沿着光线的方向移动。", translation: "Let the eye follow the direction of the light." },
      ] : [] },
    ];
  }

  get duration() {
    return this.tracks[0].clips.reduce((end, clip) => Math.max(end, clip.at + clip.length), 0);
  }

  get selectedTrack() {
    return this.tracks.find(track => track.id === this.track);
  }

  get visualTracks() {
    if (!this.trackOrder.length) return this.tracks;
    const remaining = new Map(this.tracks.map(track => [track.id, track]));
    const ordered = [];
    for (const id of this.trackOrder) {
      if (!remaining.has(id)) continue;
      ordered.push(remaining.get(id));
      remaining.delete(id);
    }
    return ordered.concat([...remaining.values()]);
  }

  get selectedClip() {
    return this.selectedTrack?.clips.find(clip => clip.id === this.selected);
  }

  videoClipAt(at) {
    return this.tracks[0].clips.find(item => item.at <= at && item.at + item.length > at)
      ?? this.tracks[0].clips.at(-1);
  }

  videoAt(at) {
    const clip = this.videoClipAt(at);
    if (clip?.stillId) return clip.source;
    return clip ? clip.source + UI.clamp(at - clip.at, 0, clip.length - 0.001) : 0;
  }

  thumbnailAt(sample) {
    const { frame, jpg } = UI;
    if (this.media === "course") return frame(sample);
    if (this.media === "desktop") return jpg("desktop");
    if (this.media === "cloud") return frame(64);
    return this.thumbnails.reduce((best, thumb) =>
      !best || Math.abs(thumb.at - sample) < Math.abs(best.at - sample) ? thumb : best, null)?.src;
  }

  subtitlesAt(at) {
    return this.tracks.filter(track => track.kind === "subtitle" && !track.muted)
      .map(track => track.clips.find(clip => at >= clip.at && at < clip.at + clip.length))
      .filter(Boolean);
  }

  snapshot() {
    return structuredClone({ tracks: this.tracks, trackOrder: this.trackOrder, markers: this.markers,
      track: this.track, selected: this.selected, at: this.at });
  }

  remember() {
    this.back.push(this.snapshot());
    if (this.back.length > 40) this.back.shift();
    this.forward = [];
  }

  history(undo) {
    const source = undo ? this.back : this.forward;
    const destination = undo ? this.forward : this.back;
    if (!source.length) return;
    destination.push(this.snapshot());
    Object.assign(this, source.pop());
    this.onChange();
  }

  choose(id, clip) {
    this.track = id;
    this.selected = clip ?? "";
    this.onChange({ timelineOnly: true });
  }

  reorderTrack(id, index) {
    const order = this.visualTracks.map(track => track.id);
    const from = order.indexOf(id);
    if (from < 0 || index < 0 || index >= order.length || index === from) return;
    this.remember();
    order.splice(from, 1);
    order.splice(index, 0, id);
    this.trackOrder = order;
    this.onChange({ timelineOnly: true });
    this.root?.querySelector(`[data-track-grip="${CSS.escape(id)}"]`)?.focus({ preventScroll: true });
  }

  addTrack(kind) {
    const prefix = kind === "audio" ? "A" : "T";
    const index = this.tracks.filter(track => track.kind === kind).length + 1;
    const id = prefix + index;
    this.remember();
    const position = this.tracks.findIndex(track => track.kind === "subtitle");
    this.tracks.splice(kind === "audio" && position >= 0 ? position : this.tracks.length, 0,
      { id, kind, name: kind === "audio" ? `音频 ${index}` : `字幕 ${index}`, clips: [] });
    this.choose(id);
    return id;
  }

  lock(id, prop) {
    const track = this.tracks.find(item => item.id === id);
    track[prop] = !track[prop];
    this.onChange();
  }

  split() {
    const changes = this.tracks.filter(track => !track.locked && (this.linked || track.id === this.track))
      .map(track => ({
        track, clip: track.clips.find(item => item.at + 0.5 / this.frameRate < this.at &&
          item.at + item.length - 0.5 / this.frameRate > this.at),
      })).filter(change => change.clip);
    if (!changes.length) {
      this.notify("把播放头移到可编辑片段的内部，再按剪切。");
      return;
    }
    this.remember();
    for (const { track, clip } of changes) {
      const offset = this.at - clip.at;
      const tail = { ...clip, id: crypto.randomUUID(), at: this.at, length: clip.length - offset };
      if ((track.kind === "video" && !clip.stillId) || track.kind === "audio") tail.source = (clip.source || 0) + offset;
      clip.length = offset;
      track.clips.splice(track.clips.indexOf(clip) + 1, 0, tail);
      if (track.id === this.track) this.selected = tail.id;
    }
    this.onChange();
  }

  remove() {
    const track = this.selectedTrack;
    const clip = this.selectedClip;
    if (!clip || track.locked) return;
    if (track.kind === "video" && track.clips.length === 1) {
      this.notify("工程需要保留一段视频。");
      return;
    }
    this.remember();
    track.clips = track.clips.filter(item => item !== clip);
    if (track.kind === "video") {
      const end = clip.at + clip.length;
      const closeGap = at => at >= end ? at - clip.length : at;
      this.packVideo();
      if (this.linked) {
        for (const other of this.tracks.slice(1).filter(item => !item.locked)) {
          other.clips = other.clips.flatMap(item => {
            const tail = item.at + item.length;
            const end = clip.at + clip.length;
            const kept = [];
            if (item.at < clip.at) kept.push({ ...item, length: Math.min(clip.at, tail) - item.at });
            if (tail > end) {
              const from = Math.max(item.at, end);
              const part = { ...item, id: kept.length ? crypto.randomUUID() : item.id, at: closeGap(from), length: tail - from };
              if (other.kind === "audio") part.source = (item.source || 0) + from - item.at;
              kept.push(part);
            }
            return kept;
          });
        }
      }
      this.markers = this.markers.filter(mark => mark.at < clip.at || mark.at >= end)
        .map(mark => ({ ...mark, at: closeGap(mark.at) }));
    }
    this.selected = "";
    this.at = Math.min(this.at, this.duration);
    this.onChange();
  }

  packVideo() {
    let at = 0;
    for (const clip of this.tracks[0].clips) {
      clip.at = at;
      at += clip.length;
    }
  }

  insertStill(still, length, caption) {
    const video = this.tracks[0];
    if (video.locked) return false;
    this.remember();
    const at = this.at;
    const clip = { id: crypto.randomUUID(), at, length, stillId: still.id, source: this.videoAt(at), name: still.title };
    const shift = track => {
      track.clips = track.clips.flatMap(item => {
        const end = item.at + item.length;
        if (item.at >= at) return [{ ...item, at: item.at + length }];
        if (end <= at) return [item];
        const offset = at - item.at;
        const tail = { ...item, id: crypto.randomUUID(), at: at + length, length: end - at };
        if ((track.kind === "video" && !item.stillId) || track.kind === "audio") tail.source = (item.source || 0) + offset;
        return [{ ...item, length: offset }, tail];
      });
    };
    shift(video);
    video.clips.push(clip);
    video.clips.sort((a, b) => a.at - b.at);
    if (this.linked) this.tracks.slice(1).filter(track => !track.locked).forEach(shift);
    this.markers.forEach(mark => { if (mark.at >= at) mark.at += length; });
    if (caption) {
      const subtitle = this.tracks.find(track => track.kind === "subtitle" && !track.locked &&
        track.clips.every(item => item.at + item.length <= at || item.at >= at + length));
      if (subtitle) {
        subtitle.clips.push({ id: crypto.randomUUID(), at, length, name: caption, translation: "" });
        subtitle.clips.sort((a, b) => a.at - b.at);
      } else {
        let n = 1;
        while (this.tracks.some(track => track.id === "T" + n)) n++;
        this.tracks.push({ id: "T" + n, kind: "subtitle", name: "静止画面字幕",
          clips: [{ id: crypto.randomUUID(), at, length, name: caption, translation: "" }] });
      }
    }
    this.track = video.id;
    this.selected = clip.id;
    this.onChange();
    return true;
  }

  addMarker(name) {
    if (this.markers.some(mark => Math.abs(this.at - mark.at) < 0.25)) return;
    this.remember();
    this.markers.push({ at: this.at, name: name || `打点 ${this.markers.length + 1}` });
    this.markers.sort((a, b) => a.at - b.at);
    this.onChange();
  }

  setTime(at) {
    this.at = UI.clamp(at, 0, this.duration);
    if (this.root) this.root.querySelector(".playhead")?.style.setProperty("--playhead", UI.pct(this.at, this.duration));
    const time = this.root?.querySelector("[data-timeline-time]");
    if (time) {
      const label = UI.time(this.at, true) + ":" + String(Math.floor(this.at % 1 * this.frameRate + 1e-8)).padStart(2, "0");
      if (time.textContent !== label) time.textContent = label;
    }
  }

  revealPlayhead() {
    if (this.zoom <= 1 || !this.duration) return;
    const viewport = this.root?.querySelector(".tracks-viewport");
    if (!viewport) return;
    const position = Math.min(viewport.scrollWidth * this.at / this.duration, viewport.scrollWidth - 1);
    const right = viewport.scrollLeft + viewport.clientWidth;
    if (position < viewport.scrollLeft || position >= right) viewport.scrollLeft = position - viewport.clientWidth * .35;
  }

  stepFrame(direction) {
    const frame = this.at * this.frameRate;
    const next = direction > 0 ? Math.floor(frame + 1e-8) + 1 : Math.ceil(frame - 1e-8) - 1;
    this.onSeek(UI.clamp(next / this.frameRate, 0, this.duration));
  }

  mount(root) {
    this.root = root;
    this.mountedMedia = this.media;
    this.render();
  }

  render() {
    if (!this.root?.isConnected) return;
    if (this.dragging) { this.redrawAfterDrag = true; return; }
    const { glyph, iconButton: ib, button: b, escape: esc, pct, text, time } = UI;
    const scrollX = this.root.querySelector(".tracks-viewport")?.scrollLeft ?? 0;
    const scrollY = this.root.querySelector(".timeline-body")?.scrollTop ?? 0;
    const seconds = this.duration || 1;
    const cursor = pct(this.at, seconds);
    const tick = seconds <= 35 ? 5 : seconds <= 200 ? 10 : 30;
    const available = Math.max(1, this.root.clientWidth - (this.root.querySelector(".track-heads")?.offsetWidth || 184)) * this.zoom;
    const active = this.selectedTrack;
    const marks = this.markers.map(mark => `<button type="button" class="seek-marker timeline-marker" data-action="timeline-seek"
      data-value="${mark.at}" title="${esc(mark.name)} · ${time(mark.at)}" aria-label="${esc(mark.name)}"
      style="--position:${pct(mark.at, seconds)}">${glyph("flag")}</button>`).join("");
    let ticks = "";
    for (let t = 0; t < seconds; t += tick / 2) {
      const major = t % tick === 0;
      ticks += `<span class="ruler-tick${major ? " major" : ""}" style="--position:${pct(t, seconds)}">${major ? `<time>${time(t)}</time>` : ""}</span>`;
    }
    const controls = `<div class="panel-head timeline-toolbar"><div class="hstack">
      <h2 class="section-title">${glyph("layers")}${esc(text("时间轴"))}</h2><i class="thin-rule"></i>
      ${ib("undo", "撤销", "undo", { disabled: !this.back.length })}
      ${ib("redo", "重做", "redo", { disabled: !this.forward.length })}
      ${ib("split", "剪切", "scissors", { variant: "mini primary" })}
      ${ib("clip-delete", "删除选中片段", "trash", { disabled: !this.selectedClip || active.locked })}
      <i class="thin-rule"></i>
      ${ib("linked", "联动编辑轨道", "link", { pressed: this.linked })}
      ${ib("snap", "吸附", "magnet", { pressed: this.snap })}
      ${ib("editor-mark", "在播放头处添加章节", "flag")}
    </div><div class="hstack">
      <time class="soft" data-timeline-time></time><i class="thin-rule"></i>
      ${ib("timeline-zoom", "缩小时间轴", "minus", { value: -0.25 })}
      <input class="timeline-zoom" type="range" min="1" max="3" step=".1" value="${this.zoom}" aria-label="时间轴缩放" data-field="timeline-zoom">
      ${ib("timeline-zoom", "放大时间轴", "plus", { value: 0.25 })}
    </div></div>`;
    const ordered = this.visualTracks;
    const rowHeaders = ordered.map(track => `<div class="track-head ${track.kind}${track.id === this.track ? " active" : ""}" data-track-row="${esc(track.id)}">
      <button type="button" class="btn flat mini track-grip" data-track-grip="${esc(track.id)}"
        title="${esc(text("拖动调整轨道顺序"))} · ↑ ↓" aria-label="${esc(text("移动轨道"))} ${esc(track.name)}"
        aria-keyshortcuts="ArrowUp ArrowDown">${glyph("grip")}</button>
      <button type="button" class="btn flat mini track-pick" data-action="track-select" data-value="${track.id}">
        <span class="prefix mono">${track.id}</span><span class="truncate">${esc(track.name)}</span></button>
      ${track.kind === "audio" ? ib("track-level", "音轨音量", "settings", { value: track.id }) : ""}
      ${track.kind === "video" ? "" : ib("track-mute", track.muted ? "启用轨道" : "静音或隐藏轨道",
        track.kind === "audio" ? (track.muted ? "mute" : "volume") : (track.muted ? "hidden" : "eye"), { value: track.id, pressed: !!track.muted })}
      ${ib("track-lock", track.locked ? "解锁轨道" : "锁定轨道", track.locked ? "lock" : "unlock", { value: track.id, pressed: !!track.locked })}
      </div>`).join("");
    const rows = ordered.map(track => `<div class="lane ${track.kind}" data-lane="${track.id}" data-ruler>
      ${track.clips.map(clip => this.renderClip(track, clip, available, seconds)).join("")}</div>`).join("");
    this.root.innerHTML = controls + `<div class="timeline-body">
      <div class="track-heads"><div class="track-ruler hstack spaced"><small>${esc(text("轨道"))}</small><div class="hstack">
      ${b("add-audio", "音频", "plus", { variant: "flat mini" })}
      ${b("add-subtitle", "字幕", "plus", { variant: "flat mini" })}</div></div>${rowHeaders}</div>
      <div class="tracks-viewport"><div class="tracks-content" style="width:${100 * this.zoom}%;--grid-step:${pct(tick, seconds)}">
      <div class="ruler" data-ruler aria-label="时间轴标尺">${ticks}${marks}</div>${rows}
      <div class="playhead" style="--playhead:${cursor}">
        <button type="button" class="playhead-handle" aria-label="播放头"></button>
      </div><div class="snap-guide" hidden></div></div></div></div>`;
    this.root.querySelector(".tracks-viewport").scrollLeft = scrollX;
    this.root.querySelector(".timeline-body").scrollTop = scrollY;
    this.setTime(this.at);
    this.root.onpointerdown = event => this.pointerDown(event);
    this.root.onkeydown = event => {
      const grip = event.target.closest("[data-track-grip]");
      if (!grip || this.dragging || !["ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const id = grip.dataset.trackGrip;
      const from = this.visualTracks.findIndex(track => track.id === id);
      this.reorderTrack(id, from + (event.key === "ArrowUp" ? -1 : 1));
    };
  }

  renderClip(track, clip, available, total) {
    const { escape: esc, time, pct } = UI;
    const width = Math.max(1, clip.length / total * available);
    let content = "";
    if (track.kind === "video") {
      const count = UI.clamp(Math.ceil(width / 94), 1, 80);
      const images = Array.from({ length: count }, (_, i) => {
        const sample = clip.source + clip.length * (i + 0.5) / count;
        const image = clip.stillId ? this.resolveStill?.(clip.stillId)?.image : this.thumbnailAt(sample);
        return image ? `<img draggable="false" src="${esc(image)}" alt="">` : "";
      }).join("");
      content = `<div class="filmstrip">${images}</div>`;
    } else if (track.kind === "audio") {
      const bars = Array.from({ length: Math.min(550, Math.ceil(width / 4)) }, (_, i) => {
        const strength = 9 + Math.abs(Math.sin(i * 1.21 + clip.at) * Math.cos(i * 0.16)) * 78;
        return `<i style="height:${Math.round(strength)}%"></i>`;
      }).join("");
      content = `<div class="waveform" aria-hidden="true">${bars}</div>`;
    }
    const label = track.kind === "video" && !clip.stillId ? `${time(clip.at)} – ${time(clip.at + clip.length)}` : clip.name;
    return `<button type="button" class="clip ${track.kind}${clip.id === this.selected ? " selected" : ""}"
      style="--left:${pct(clip.at, total)};--width:${pct(clip.length, total)}" data-clip="${clip.id}" data-track="${track.id}"
      title="${esc(label)}" aria-label="${esc(label)}" aria-pressed="${clip.id === this.selected}">${content}
      <span class="clip-label ${track.kind === "video" ? "mono" : ""}">${esc(label)}</span></button>`;
  }

  pointerDown(event) {
    if (event.button !== 0 || this.dragging) return;
    const trackGrip = event.target.closest("[data-track-grip]");
    if (trackGrip) {
      this.dragTrack(event, trackGrip);
      return;
    }
    if (event.target.closest("[data-action]")) return;
    const handle = event.target.closest(".playhead-handle");
    const element = event.target.closest(".clip");
    const lane = event.target.closest("[data-ruler]");
    if ((!element && !lane && !handle) || !this.duration) return;
    const content = this.root.querySelector(".tracks-content");
    const seconds = this.duration;
    if (!element) {
      event.preventDefault();
      const guide = content.querySelector(".snap-guide");
      const edges = this.clipEdges().filter(edge => edge >= 0 && edge <= seconds);
      const seek = (clientX, snapToEdges = false) => {
        const rect = content.getBoundingClientRect();
        if (!rect.width) return;
        const at = UI.clamp((clientX - rect.left) / rect.width * seconds, 0, seconds);
        let target;
        if (this.snap && snapToEdges) {
          let nearest = 8 * seconds / rect.width;
          for (const edge of edges) {
            const distance = Math.abs(edge - at);
            if (distance >= nearest) continue;
            target = edge;
            nearest = distance;
          }
        }
        guide.hidden = target === undefined;
        if (target !== undefined) guide.style.left = UI.pct(target, seconds);
        this.onSeek(target ?? at);
      };
      if (!handle) seek(event.clientX, true);
      const grabOffset = handle ? event.clientX - (content.getBoundingClientRect().left +
        content.getBoundingClientRect().width * this.at / seconds) : 0;
      const grab = handle ?? lane;
      content.querySelector(".playhead-handle").focus({ preventScroll: true });
      this.dragPointer(event, grab, ev => seek(ev.clientX - grabOffset, true), () => {
        guide.hidden = true;
      });
      return;
    }
    this.dragClip(event, element, content);
  }

  dragTrack(event, grip) {
    const body = grip.closest(".timeline-body");
    const source = grip.closest("[data-track-row]");
    const id = grip.dataset.trackGrip;
    const lanes = new Map([...body.querySelectorAll("[data-lane]")].map(lane => [lane.dataset.lane, lane]));
    const heads = [...body.querySelectorAll("[data-track-row]")].filter(head => head !== source);
    if (!heads.length) return;
    event.preventDefault();
    grip.focus({ preventScroll: true });
    let target, insertAt, cursor = event.clientY;
    let started = false;
    let scrollFrame = 0;
    const mark = (head, cls, active) => {
      if (!head) return;
      head.classList.toggle(cls, active);
      lanes.get(head.dataset.trackRow)?.classList.toggle(cls, active);
    };
    const clearTarget = () => {
      mark(target, "track-drop-before", false);
      mark(target, "track-drop-after", false);
    };
    const pickTarget = () => {
      const boundary = heads.findIndex(head => {
        const { top, height } = head.getBoundingClientRect();
        return cursor < top + height / 2;
      });
      const next = boundary < 0 ? heads.at(-1) : heads[boundary];
      clearTarget();
      target = next;
      insertAt = boundary < 0 ? heads.length : boundary;
      mark(target, boundary < 0 ? "track-drop-after" : "track-drop-before", true);
    };
    const scroll = () => {
      scrollFrame = 0;
      if (!body.isConnected || !this.dragging) return;
      const { top, bottom } = body.getBoundingClientRect();
      const step = cursor < top + 22 ? -14 : cursor > bottom - 22 ? 14 : 0;
      const previous = body.scrollTop;
      if (step) body.scrollTop += step;
      pickTarget();
      if (body.scrollTop !== previous) scrollFrame = requestAnimationFrame(scroll);
    };
    const move = pointer => {
      cursor = pointer.clientY;
      if (!started && Math.abs(cursor - event.clientY) < 4) return;
      started = true;
      mark(source, "track-reordering", true);
      if (!scrollFrame) scroll();
    };
    this.dragPointer(event, grip, move, pointer => {
      if (scrollFrame) cancelAnimationFrame(scrollFrame);
      if (!started) return;
      if (pointer.type === "pointerup") {
        cursor = pointer.clientY;
        pickTarget();
      }
      mark(source, "track-reordering", false);
      clearTarget();
      if (pointer.type === "pointerup") this.reorderTrack(id, insertAt);
    });
  }

  clipEdges(excludedId) {
    return this.tracks.flatMap(track => track.clips.filter(clip => clip.id !== excludedId)
      .flatMap(clip => [clip.at, clip.at + clip.length]));
  }

  dragPointer(event, target, onMove, onEnd) {
    const document = target.ownerDocument;
    const pointerId = event.pointerId;
    const root = this.root;
    this.dragging = true;
    target.setPointerCapture(pointerId);
    const move = pointer => {
      if (pointer.pointerId === pointerId && root.isConnected) onMove(pointer);
    };
    const finish = pointer => {
      if (pointer.pointerId !== pointerId) return;
      document.removeEventListener("pointermove", move, true);
      document.removeEventListener("pointerup", finish, true);
      document.removeEventListener("pointercancel", finish, true);
      target.removeEventListener("lostpointercapture", finish);
      this.dragging = false;
      const redraw = this.redrawAfterDrag;
      this.redrawAfterDrag = false;
      if (this.root === root && root.isConnected) onEnd(pointer);
      if (redraw) this.render();
    };
    document.addEventListener("pointermove", move, true);
    document.addEventListener("pointerup", finish, true);
    document.addEventListener("pointercancel", finish, true);
    target.addEventListener("lostpointercapture", finish);
  }

  dragClip(event, element, content) {
    event.preventDefault();
    const track = this.tracks.find(item => item.id === element.dataset.track);
    const clip = track.clips.find(item => item.id === element.dataset.clip);
    const total = this.duration;
    const scale = content.getBoundingClientRect().width / total;
    const guide = content.querySelector(".snap-guide");
    const targets = [this.at, ...this.markers.map(mark => mark.at), ...this.clipEdges(clip.id)];
    let candidate = clip.at;
    let dragged = false;
    const move = ev => {
      const delta = ev.clientX - event.clientX;
      if (track.locked || (!dragged && Math.abs(delta) < 4)) return;
      dragged = true;
      element.classList.add("dragging");
      candidate = UI.clamp(clip.at + delta / scale, 0, total - clip.length);
      let match;
      let nearest = 8 / scale;
      if (this.snap) {
        for (const target of targets) {
          for (const shift of [0, clip.length]) {
            const left = target - shift;
            const distance = Math.abs(left - candidate);
            if (distance < nearest && left >= 0 && left + clip.length <= total + 0.001) {
              match = [left, target];
              nearest = distance;
            }
          }
        }
      }
      guide.hidden = !match;
      if (match) {
        candidate = match[0];
        guide.style.left = UI.pct(match[1], total);
      }
      if (track.kind !== "video") {
        const clamped = this.availableClipStart(track, clip, candidate);
        if (clamped !== candidate) guide.hidden = true;
        candidate = clamped;
      }
      element.style.setProperty("--left", UI.pct(candidate, total));
    };
    const end = ev => {
      if (ev.type !== "pointerup") {
        this.render();
        return;
      }
      if (dragged) {
        this.remember();
        if (track.kind === "video") this.moveVideo(clip, candidate);
        else {
          clip.at = candidate;
          track.clips.sort((a, b) => a.at - b.at);
        }
      }
      const same = track.id === this.track;
      this.track = track.id;
      this.selected = clip.id;
      this.onChange({ timelineOnly: !dragged && same });
    };
    this.dragPointer(event, element, move, end);
  }

  availableClipStart(track, clip, candidate) {
    const others = track.clips.filter(item => item !== clip);
    const max = this.duration - clip.length;
    const points = [candidate, clip.at, 0, max, ...others.flatMap(item => [item.at + item.length, item.at - clip.length])];
    return points.filter(at => at >= 0 && at <= max
      && others.every(item => at + clip.length <= item.at + 1e-6 || at >= item.at + item.length - 1e-6))
      .sort((a, b) => Math.abs(a - candidate) - Math.abs(b - candidate))[0] ?? clip.at;
  }

  moveVideo(clip, candidate) {
    const original = this.tracks[0].clips.map(item => ({ ...item }));
    const reordered = this.tracks[0].clips.filter(item => item !== clip);
    const direction = candidate >= clip.at ? clip.length * 0.65 : 0;
    const insertBefore = reordered.findIndex(item => candidate + direction < item.at + item.length / 2);
    reordered.splice(insertBefore < 0 ? reordered.length : insertBefore, 0, clip);
    this.tracks[0].clips = reordered;
    this.packVideo();
    const shiftFor = at => {
      const previous = original.find(item => at >= item.at && at < item.at + item.length);
      return previous ? reordered.find(item => item.id === previous.id).at - previous.at : 0;
    };
    this.markers.forEach(mark => { mark.at += shiftFor(mark.at); });
    this.markers.sort((a, b) => a.at - b.at);
    if (this.linked) {
      for (const track of this.tracks.slice(1).filter(item => !item.locked)) {
        track.clips = track.clips.flatMap(item => original.flatMap(segment => {
          const from = Math.max(item.at, segment.at);
          const to = Math.min(item.at + item.length, segment.at + segment.length);
          if (to - from <= .001) return [];
          const part = { ...item, id: crypto.randomUUID(), at: from + shiftFor(from), length: to - from };
          if (track.kind === "audio") part.source = (item.source || 0) + from - item.at;
          return [part];
        })).sort((a, b) => a.at - b.at);
      }
    }
  }
}
