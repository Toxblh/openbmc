// Virtual media: the browser serves a disk image to the host over NBD.
// Slot 0 is a removable USB disk (read-write capable), slot 1 a USB CD-ROM.

import { api, h, toast, fmtBytes } from "../api.js";
import { NbdServer, BlobBackend, OverlayBackend, FileHandleBackend, OpfsBackend } from "../lib/nbd-server.js";

const hasPicker = typeof window.showOpenFilePicker === "function";
const hasOpfs = !!(navigator.storage && navigator.storage.getDirectory);

const SLOTS = [
  { id: 0, title: "USB-диск", hint: "Съёмный диск. Поддерживает запись, если источник позволяет." },
  { id: 1, title: "USB CD-ROM", hint: "Оптический привод, только чтение (ISO-образы)." },
];

const MODES = [
  { id: "file-ro", label: "Локальный файл — только чтение", ok: () => true },
  { id: "file-rw", label: "Локальный файл — чтение и запись", ok: () => hasPicker, why: "нужен Chromium-браузер (File System Access API)" },
  { id: "snapshot", label: "Локальный файл — запись в снимок (в памяти браузера)", ok: () => true },
  { id: "opfs", label: "Образ из хранилища браузера", ok: () => hasOpfs, why: "браузер не поддерживает OPFS" },
];

let root;
const slots = SLOTS.map((s) => ({ ...s, server: null, backend: null, file: null, handle: null, mode: s.id === 1 ? "file-ro" : "file-rw" }));

function onBeforeUnload(e) {
  if (slots.some((s) => s.server)) {
    e.preventDefault();
    e.returnValue = "";
  }
}

export async function mount(el) {
  root = el;
  window.addEventListener("beforeunload", onBeforeUnload);
  if (!hasPicker) slots[0].mode = hasOpfs ? "opfs" : "file-ro";
  el.append(
    h("h2", {}, "Виртуальные носители"),
    h(
      "p",
      { class: "note" },
      "Образ отдаётся платформе прямо из браузера: пока носитель подключён, эта вкладка должна оставаться открытой. ",
      "Для записи с платформы выберите режим «чтение и запись» или образ в хранилище браузера. ",
      "Перед отключением диска с записью отмонтируйте его на платформе (sync/umount): так все данные гарантированно сохранятся.",
    ),
    h("div", { class: "grid section" }, slots.map(renderSlot)),
    hasOpfs ? renderLibrary() : h("p", { class: "note warn" }, "Браузер не поддерживает хранилище образов (OPFS)."),
  );
  if (hasOpfs) refreshLibrary();
}

function renderSlot(s) {
  const modes = MODES.filter((m) => s.id === 0 || m.id === "file-ro" || m.id === "opfs");
  s.el = {
    mode: h(
      "select",
      {},
      modes.map((m) => h("option", { value: m.id, disabled: !m.ok() }, m.label + (m.ok() ? "" : ` (${m.why})`))),
    ),
    file: h("input", { type: "file" }),
    pick: h("button", { class: "btn" }, "Выбрать файл…"),
    opfs: h("select", {}),
    rw: h("label", { class: "inline" }, h("input", { type: "checkbox", checked: s.id === 0 }), "разрешить запись"),
    source: h("div", { class: "muted small" }, "Источник не выбран"),
    connect: h("button", { class: "btn primary" }, "Подключить"),
    disconnect: h("button", { class: "btn danger hidden" }, "Отключить"),
    save: h("button", { class: "btn hidden", title: "Записать изменения в файл" }, "Сохранить"),
    download: h("button", { class: "btn hidden", title: "Скачать образ со всеми изменениями" }, "Скачать образ"),
    state: h("div", { class: "state" }, h("span", { class: "led off" }), " отключён"),
    stats: h("div", { class: "muted small mono" }, ""),
  };
  s.el.mode.value = s.mode;
  const e = s.el;
  const sourceRow = h("div", { class: "row" }, e.file, e.pick, e.opfs, e.rw);
  const card = h(
    "div",
    { class: "card slot" },
    h("h3", {}, s.title, h("span", { class: "muted small" }, `/vm/0/${s.id}`)),
    h("div", { class: "muted small" }, s.hint),
    e.mode,
    sourceRow,
    e.source,
    e.state,
    e.stats,
    h("div", { class: "row" }, e.connect, e.disconnect, e.save, e.download),
  );
  if (s.id === 1) e.rw.classList.add("hidden");

  const syncMode = () => {
    s.mode = e.mode.value;
    e.file.classList.toggle("hidden", !(s.mode === "file-ro" || s.mode === "snapshot"));
    e.pick.classList.toggle("hidden", s.mode !== "file-rw");
    e.opfs.classList.toggle("hidden", s.mode !== "opfs");
    e.rw.classList.toggle("hidden", s.mode !== "opfs" || s.id === 1);
    updateSource(s);
  };
  e.mode.addEventListener("change", syncMode);
  e.file.addEventListener("change", () => {
    s.file = e.file.files[0] || null;
    updateSource(s);
  });
  e.pick.addEventListener("click", async () => {
    try {
      const [handle] = await window.showOpenFilePicker({ multiple: false });
      // ask for write permission right away (user gesture is present)
      const perm = await handle.requestPermission({ mode: "readwrite" });
      if (perm !== "granted") throw new Error("нет разрешения на запись в файл");
      s.handle = handle;
      updateSource(s);
    } catch (err) {
      if (err.name !== "AbortError") toast(err.message, "err");
    }
  });
  e.opfs.addEventListener("change", () => updateSource(s));
  e.connect.addEventListener("click", () => connect(s));
  e.disconnect.addEventListener("click", () => disconnect(s));
  e.save.addEventListener("click", async () => {
    try {
      await s.backend.flush();
      toast("Изменения сохранены в файл", "ok");
      updateStats(s);
    } catch (err) {
      toast(`Ошибка сохранения: ${err.message}`, "err");
    }
  });
  e.download.addEventListener("click", async () => {
    try {
      const blob = await s.backend.toBlob();
      const a = h("a", { href: URL.createObjectURL(blob), download: s.backend.name.replace(/(\.[^.]*)?$/, "-modified$1") });
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 60000);
    } catch (err) {
      toast(err.message, "err");
    }
  });
  syncMode();
  return card;
}

function updateSource(s) {
  const e = s.el;
  let text = "Источник не выбран";
  if ((s.mode === "file-ro" || s.mode === "snapshot") && s.file) text = `${s.file.name} — ${fmtBytes(s.file.size)}`;
  if (s.mode === "file-rw" && s.handle) text = `${s.handle.name} (чтение и запись)`;
  if (s.mode === "opfs" && e.opfs.value) text = `${e.opfs.value} из хранилища браузера`;
  e.source.textContent = text;
}

async function makeBackend(s) {
  switch (s.mode) {
    case "file-ro":
      if (!s.file) throw new Error("выберите файл");
      return new BlobBackend(s.file);
    case "snapshot":
      if (!s.file) throw new Error("выберите файл");
      return new OverlayBackend(new BlobBackend(s.file));
    case "file-rw":
      if (!s.handle) throw new Error("выберите файл");
      return FileHandleBackend.open(s.handle);
    case "opfs": {
      const name = s.el.opfs.value;
      if (!name) throw new Error("выберите образ в хранилище");
      if (slots.some((o) => o !== s && o.server && o.opfsName === name)) throw new Error("образ уже подключён в другом слоте");
      s.opfsName = name;
      const ro = s.id === 1 || !s.el.rw.querySelector("input").checked;
      return OpfsBackend.open(name, { readOnly: ro });
    }
  }
  throw new Error("неизвестный режим");
}

async function connect(s) {
  if (s.server) return;
  let backend;
  try {
    backend = await makeBackend(s);
  } catch (err) {
    toast(`Носитель: ${err.message}`, "err");
    return;
  }
  if (s.id === 1) {
    backend.readOnly = true;
    // f_mass_storage refuses CD-ROM media shorter than 300 2048-byte sectors
    if (backend.size < 300 * 2048) {
      toast("Образ слишком мал для CD-ROM (минимум 600 КиБ). Подключите его как USB-диск.", "err");
      await backend.close();
      return;
    }
  }
  if (backend.size === 0) {
    toast("Образ пустой (0 байт)", "err");
    await backend.close();
    return;
  }
  s.backend = backend;
  const ws = api.websocket(`/vm/0/${s.id}`);
  const srv = (s.server = new NbdServer(ws, backend, { readOnly: backend.readOnly }));
  setState(s, "warn", "подключение…");
  srv.addEventListener("connected", (ev) => {
    setState(s, "on", ev.detail.readOnly ? "подключён (только чтение)" : "подключён (чтение и запись)");
    toast(`${s.title}: образ ${backend.name} подключён к платформе`, "ok");
  });
  srv.addEventListener("stats", () => updateStats(s));
  srv.addEventListener("error", (ev) => toast(`${s.title}: ${ev.detail.message}`, "err"));
  srv.addEventListener("close", async (ev) => {
    const was = s.server;
    s.server = null;
    s.opfsName = null;
    setState(s, "off", ev.detail.reason ? `отключён: ${ev.detail.reason}` : "отключён");
    try {
      await backend.close();
    } catch (err) {
      toast(`${s.title}: ${err.message}`, "err");
    }
    updateStats(s, was);
    s.el.connect.classList.remove("hidden");
    s.el.disconnect.classList.add("hidden");
    s.el.mode.disabled = false;
    if (s.mode === "opfs") refreshLibrary();
  });
  s.el.connect.classList.add("hidden");
  s.el.disconnect.classList.remove("hidden");
  s.el.mode.disabled = true;
  updateStats(s);
}

async function disconnect(s) {
  if (!s.server) return;
  if (!s.backend.readOnly && !confirm("Отключить носитель? Убедитесь, что он отмонтирован на платформе.")) return;
  s.server.stop();
}

function setState(s, led, text) {
  s.el.state.replaceChildren(h("span", { class: `led ${led}` }), " ", text);
}

function updateStats(s, server = s.server) {
  const e = s.el;
  const be = s.backend;
  if (!server || !be) {
    e.stats.textContent = "";
  } else {
    const st = server.stats;
    e.stats.textContent =
      `${be.describe()} · размер ${fmtBytes(be.size)}\n` +
      `чтение: ${st.reads} оп / ${fmtBytes(st.readBytes)} · запись: ${st.writes} оп / ${fmtBytes(st.writtenBytes)} · flush: ${st.flushes}` +
      (st.errors ? ` · ошибок: ${st.errors}` : "");
    e.stats.style.whiteSpace = "pre-line";
  }
  const dirty = be && be.dirty && be.dirty.size > 0;
  e.save.classList.toggle("hidden", !(be instanceof FileHandleBackend && dirty));
  e.download.classList.toggle("hidden", !(be instanceof OverlayBackend && !(be instanceof FileHandleBackend) && dirty));
}

/* ------------------------------------------------------------------ */
/* image library in OPFS                                                */

let lib;

async function imagesDir() {
  const rootDir = await navigator.storage.getDirectory();
  return rootDir.getDirectoryHandle("images", { create: true });
}

function renderLibrary() {
  lib = {
    table: h("tbody"),
    quota: h("span", { class: "muted small" }),
    newName: h("input", { placeholder: "имя.img", size: 14 }),
    newSize: h("input", { type: "number", min: 1, value: 64, size: 6, title: "МиБ" }),
    importFile: h("input", { type: "file", class: "hidden" }),
    progress: h("div", { class: "progress hidden" }, h("div")),
  };
  const create = h("button", { class: "btn" }, "Создать пустой");
  const imp = h("button", { class: "btn" }, "Импорт файла…");
  create.addEventListener("click", createImage);
  imp.addEventListener("click", () => lib.importFile.click());
  lib.importFile.addEventListener("change", importImage);
  return h(
    "div",
    { class: "card section" },
    h("h3", {}, "Хранилище образов в браузере", lib.quota),
    h(
      "p",
      { class: "muted small" },
      "Образы хранятся в приватной файловой системе браузера (OPFS) для этого BMC и поддерживают полноценную запись. ",
      "Удобно для обмена файлами с платформой: создайте пустой образ, отформатируйте его на платформе, скачайте после работы.",
    ),
    h("div", { class: "row" }, lib.newName, lib.newSize, h("span", { class: "muted" }, "МиБ"), create, h("span", { class: "spacer" }), imp, lib.importFile),
    lib.progress,
    h("div", { class: "imglist" }, h("table", { class: "tbl" }, h("thead", {}, h("tr", {}, h("th", {}, "Образ"), h("th", {}, "Размер"), h("th", {}, "Изменён"), h("th", {}))), lib.table)),
  );
}

let libGeneration = 0;

export async function refreshLibrary() {
  if (!lib) return;
  // several refreshes may overlap (page show, create, import): only the
  // latest one is allowed to render, otherwise a stale listing could win
  const gen = ++libGeneration;
  const dir = await imagesDir();
  const items = [];
  for await (const [name, handle] of dir.entries()) {
    if (handle.kind !== "file") continue;
    try {
      const f = await handle.getFile();
      items.push({ name, size: f.size, mtime: f.lastModified });
    } catch (e) {
      items.push({ name, size: NaN, mtime: 0, busy: true }); // locked by a sync handle
    }
  }
  items.sort((a, b) => a.name.localeCompare(b.name));
  if (gen !== libGeneration) return;
  lib.table.replaceChildren(
    ...(items.length
      ? items.map((it) => {
          const inUse = slots.some((s) => s.server && s.opfsName === it.name);
          const dl = h("button", { class: "btn small", disabled: inUse }, "Скачать");
          const del = h("button", { class: "btn small danger", disabled: inUse }, "Удалить");
          dl.addEventListener("click", () => downloadImage(it.name));
          del.addEventListener("click", () => deleteImage(it.name));
          return h(
            "tr",
            {},
            h("td", { class: "mono" }, it.name, inUse ? h("span", { class: "badge ok" }, " подключён") : null),
            h("td", {}, it.busy ? "используется" : fmtBytes(it.size)),
            h("td", { class: "small muted" }, it.mtime ? new Date(it.mtime).toLocaleString() : ""),
            h("td", { class: "row end" }, dl, del),
          );
        })
      : [h("tr", {}, h("td", { colspan: 4, class: "empty" }, "Образов пока нет"))]),
  );
  for (const s of slots) {
    const sel = s.el.opfs;
    const cur = sel.value;
    sel.replaceChildren(h("option", { value: "" }, "— образ —"), ...items.map((it) => h("option", { value: it.name }, `${it.name} (${fmtBytes(it.size)})`)));
    sel.value = items.some((i) => i.name === cur) ? cur : "";
    updateSource(s);
  }
  if (navigator.storage.estimate) {
    const est = await navigator.storage.estimate();
    lib.quota.textContent = `занято ${fmtBytes(est.usage)} из ${fmtBytes(est.quota)}`;
  }
}

function validName(name) {
  return /^[\w.\-а-яА-ЯёЁ ]{1,100}$/.test(name) && name !== "." && name !== "..";
}

async function createImage() {
  const name = lib.newName.value.trim() || `disk-${Date.now() % 100000}.img`;
  const mib = Number(lib.newSize.value);
  if (!validName(name)) return toast("Недопустимое имя образа", "err");
  if (!(mib > 0 && mib <= 1024 * 1024)) return toast("Укажите размер в МиБ", "err");
  try {
    const dir = await imagesDir();
    try {
      await dir.getFileHandle(name);
      return toast("Образ с таким именем уже есть", "err");
    } catch (e) {
      /* not found: ok */
    }
    const fh = await dir.getFileHandle(name, { create: true });
    const w = await fh.createWritable();
    await w.truncate(mib * 1024 * 1024);
    await w.close();
    lib.newName.value = "";
    toast(`Создан образ ${name}`, "ok");
    refreshLibrary();
  } catch (e) {
    toast(`Не удалось создать образ: ${e.message}`, "err");
  }
}

async function importImage() {
  const file = lib.importFile.files[0];
  lib.importFile.value = "";
  if (!file) return;
  if (!validName(file.name)) return toast("Недопустимое имя файла", "err");
  const bar = lib.progress;
  try {
    const dir = await imagesDir();
    const fh = await dir.getFileHandle(file.name, { create: true });
    const w = await fh.createWritable();
    bar.classList.remove("hidden");
    let done = 0;
    const reader = file.stream().getReader();
    for (;;) {
      const { value, done: end } = await reader.read();
      if (end) break;
      await w.write(value);
      done += value.length;
      bar.firstChild.style.width = `${((done / file.size) * 100).toFixed(1)}%`;
    }
    await w.close();
    toast(`Импортирован ${file.name}`, "ok");
  } catch (e) {
    toast(`Импорт не удался: ${e.message}`, "err");
  } finally {
    bar.classList.add("hidden");
    bar.firstChild.style.width = "0";
    refreshLibrary();
  }
}

async function downloadImage(name) {
  const dir = await imagesDir();
  const f = await (await dir.getFileHandle(name)).getFile();
  const a = h("a", { href: URL.createObjectURL(f), download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}

async function deleteImage(name) {
  if (!confirm(`Удалить образ ${name}?`)) return;
  const dir = await imagesDir();
  await dir.removeEntry(name);
  refreshLibrary();
}

export function show() {
  if (hasOpfs) refreshLibrary();
}

// test hook: lets automated tests drive the page without file pickers
window.reimuMedia = { slots, connect, disconnect, refreshLibrary, imagesDir };
