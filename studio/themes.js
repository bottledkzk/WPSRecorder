"use strict";

const ThemeManager = (() => {
  const storageKey = "wps-recorder-studio-theme-v1";
  const defaultThemeId = "botanical-sage";

  const themes = [
    {
      id: "default",
      name: "默认深色",
      enName: "Classic Dark",
      badge: "原生经典",
      tagline: "WPS Recorder 2.3.8 原生专业深灰暗色工作台",
      scheme: "dark",
      preview: "",
      swatches: ["#191c22", "#23262c", "#2e5caa", "#5bd6aa", "#ecc078"],
    },
    {
      id: "default-light",
      name: "默认浅色",
      enName: "Classic Light",
      badge: "原生经典",
      tagline: "冷白与浅灰界面，配以蓝色操作控件",
      scheme: "light",
      preview: "",
      swatches: ["#eef2f7", "#ffffff", "#365d9e", "#e3edfc", "#263244"],
    },
    {
      id: "botanical-sage",
      name: "慕夏苔绿",
      enName: "Botanical Sage",
      badge: "花境卷轴",
      tagline: "温润羊皮纸与沉静苔绿，新艺术花环纹样与清透模块",
      scheme: "light",
      preview: "assets/themes/botanical-sage/preview.webp",
      swatches: ["#f4f0e4", "#fcfbf7", "#3b564d", "#8b6246", "#d6c5a3"],
    },
    {
      id: "wisteria-ribbon",
      name: "雾紫轻纱",
      enName: "Wisteria Ribbon",
      badge: "流丝清境",
      tagline: "雪雾淡紫与手绘流丝缎带，晶莹留白与深梅紫柔光",
      scheme: "light",
      preview: "assets/themes/wisteria-ribbon/preview.webp",
      swatches: ["#f4f0f9", "#ffffff", "#624d85", "#a85378", "#dfd2f0"],
    },
    {
      id: "spring-creek",
      name: "春溪樱雨",
      enName: "Spring Sakura Creek",
      badge: "晴空山溪",
      tagline: "明媚春空、樱花连翘与青苔溪石，手绘淡彩暖白工作台",
      scheme: "light",
      preview: "assets/themes/spring-creek/preview.webp",
      swatches: ["#eef6fb", "#fffefa", "#2f6f6a", "#de5c7d", "#e3b42b"],
    },
    {
      id: "golden-wheat",
      name: "麦浪晴光",
      enName: "Golden Wheat Sunlit",
      badge: "晴金麦田",
      tagline: "逆光金穗、矢车菊雾蓝与朱红虞美人，温暖田园晴昼质感",
      scheme: "light",
      preview: "assets/themes/golden-wheat/preview.webp",
      swatches: ["#faf4e5", "#fffef9", "#415e95", "#de5a3b", "#d99d30"],
    },
    {
      id: "riverside-sakura",
      name: "碧水樱影",
      enName: "Riverside Sakura Shade",
      badge: "碧溪垂樱",
      tagline: "青碧河面、垂枝淡樱与石板斑驳蓝紫树影，清幽水墨画境",
      scheme: "light",
      preview: "assets/themes/riverside-sakura/preview.webp",
      swatches: ["#ecf4f7", "#fcfdff", "#2e7379", "#6582bf", "#eab4ce"],
    },
    {
      id: "snow-sakura",
      name: "雪樱绯风",
      enName: "Snow Sakura Flutter",
      badge: "绯风吹雪",
      tagline: "澄净雪白留白、飞扬绯红开衫与水手靛紫，高调轻盈光斑",
      scheme: "light",
      preview: "assets/themes/snow-sakura/preview.webp",
      swatches: ["#faf8fc", "#ffffff", "#c93363", "#3d375c", "#c9cde8"],
    },
    {
      id: "sakura-hillside",
      name: "樱坂晨雾",
      enName: "Sakura Hillside Mist",
      badge: "樱坡浅雾",
      tagline: "柔雾樱坂、暖珊瑚粉开衫与淡紫街景薄霭，温柔怀旧晨光",
      scheme: "light",
      preview: "assets/themes/sakura-hillside/preview.webp",
      swatches: ["#faf0f4", "#fffcfd", "#8a688d", "#de6d7b", "#f7d5e0"],
    },
    {
      id: "dusk-park",
      name: "暮光林荫",
      enName: "Dusk Park Gouache",
      badge: "水粉暮园",
      tagline: "厚涂水粉黄昏树荫、深普鲁士蓝卫衣与粉青发丝落叶",
      scheme: "light",
      preview: "assets/themes/dusk-park/preview.webp",
      swatches: ["#faf3e4", "#fffef9", "#17477e", "#df6996", "#a4b82e"],
    },
    {
      id: "azure-sky",
      name: "晴空飞鸽",
      enName: "Azure Sky Doves",
      badge: "澄空云海",
      tagline: "澄澈天青云海、飞翔白鸽与半透雏菊披风，清透夏日流风",
      scheme: "light",
      preview: "assets/themes/azure-sky/preview.webp",
      swatches: ["#eaf7fd", "#ffffff", "#0d88ba", "#d64d62", "#e0a928"],
    },
    {
      id: "autumn-ginkgo",
      name: "金杏和风",
      enName: "Autumn Ginkgo Kimono",
      badge: "秋杏锦织",
      tagline: "暖金银杏光斑、朱漆和服织纹与银青双马尾，温润和风秋韵",
      scheme: "light",
      preview: "assets/themes/autumn-ginkgo/preview.webp",
      swatches: ["#faf2e3", "#fffef9", "#b8422d", "#5c7c8d", "#e0a522"],
    },
    {
      id: "cobalt-seaside",
      name: "湛海听风",
      enName: "Cobalt Seaside Walk",
      badge: "湛蓝海风",
      tagline: "深钴蓝夏日积云、青碧海平线、白鸽与紫红棱镜流光，澄澈辽阔",
      scheme: "light",
      preview: "assets/themes/cobalt-seaside/preview.webp",
      swatches: ["#e8f1fb", "#ffffff", "#1858be", "#12a6b8", "#cc4494"],
    },
  ];

  function byId(id) {
    return themes.find(item => item.id === id) || themes.find(item => item.id === defaultThemeId);
  }

  function readSaved() {
    try {
      const saved = localStorage.getItem(storageKey);
      if (saved && themes.some(item => item.id === saved)) return saved;
    } catch {}
    return defaultThemeId;
  }

  let activeId = readSaved();

  function apply(id, persist = true) {
    const theme = byId(id);
    activeId = theme.id;
    const root = document.documentElement;
    if (theme.id === "default") {
      delete root.dataset.theme;
    } else {
      root.dataset.theme = theme.id;
    }
    root.dataset.themeScheme = theme.scheme;
    const meta = document.querySelector('meta[name="color-scheme"]');
    if (meta) meta.setAttribute("content", theme.scheme);
    if (persist) {
      try { localStorage.setItem(storageKey, theme.id); } catch {}
    }
    document.dispatchEvent(new CustomEvent("themechange", { detail: theme }));
    return theme;
  }

  function current() {
    return byId(activeId);
  }

  function renderSettings() {
    const esc = UI.escape;
    const label = text => esc(UI.text(text));
    const active = current().id;
    const cards = themes.map(theme => {
      const selected = theme.id === active;
      const art = theme.preview
        ? `<img class="theme-card-img" src="${esc(theme.preview)}" alt="${esc(theme.name)}">`
        : `<div class="theme-card-native" aria-hidden="true">
            <span class="native-bar"></span>
            <span class="native-stage"></span>
            <span class="native-docks"><i></i><i></i><i></i></span>
          </div>`;
      const dots = theme.swatches.map(color =>
        `<span class="theme-swatch" style="background:${esc(color)}" title="${esc(color)}"></span>`
      ).join("");
      return `<button type="button" class="theme-card${selected ? " is-active" : ""}"
        data-action="set-theme" data-value="${esc(theme.id)}" aria-pressed="${selected}">
        <div class="theme-card-art">
          ${art}
          <span class="theme-card-badge">${esc(theme.badge)}</span>
          <span class="theme-card-check"${selected ? "" : " hidden"}>${UI.glyph("check")}<span>${label("当前使用")}</span></span>
        </div>
        <div class="theme-card-body">
          <div class="theme-card-head">
            <strong class="theme-card-title">${esc(theme.name)}</strong>
            <span class="theme-card-en">${esc(theme.enName)}</span>
          </div>
          <p class="theme-card-desc">${esc(theme.tagline)}</p>
          <div class="theme-swatches" aria-hidden="true">${dots}</div>
        </div>
      </button>`;
    }).join("");
    return `<div class="theme-settings-panel">
      <div class="theme-settings-intro">
        <strong>${label("外观主题")}</strong>
      </div>
      <div class="theme-gallery" role="list" aria-label="${label("可选主题")}">${cards}</div>
    </div>`;
  }

  function updateSettingsSelection(settings) {
    for (const card of settings.querySelectorAll(".theme-gallery .theme-card")) {
      const selected = card.dataset.value === activeId;
      card.classList.toggle("is-active", selected);
      card.setAttribute("aria-pressed", String(selected));
      card.querySelector(".theme-card-check").hidden = !selected;
    }
  }

  apply(activeId, false);

  return { themes, apply, current, renderSettings, updateSettingsSelection };
})();
