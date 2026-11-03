import { api, h, toast } from "../api.js";

let root, timer;

const SEV = { OK: "ok", Warning: "warn", Critical: "err" };
const BMC = "bmc-journal";
const EV = "bmc-events";
const LOGS = "/xyz/openbmc_project/reimu/logs";
const logsCall = async (name, data) => JSON.parse((await api.post(`${LOGS}/action/${name}`, { data })).data);

export async function mount(el) {
  root = el;
  el.append(
    h(
      "div",
      { class: "row section log-tools" },
      h("h2", {}, "Журнал"),
      h("span", { class: "spacer" }),
      h("select", { id: "log-svc" }, h("option", { value: BMC }, "BMC (системный журнал)"), h("option", { value: EV }, "BMC: события (сохраняются при перезагрузке)")),
      h("select", { id: "log-prio", title: "Важность" }, h("option", { value: "7" }, "Все"), h("option", { value: "5" }, "Важные"), h("option", { value: "4" }, "Предупреждения"), h("option", { value: "3" }, "Ошибки")),
      h("label", { class: "inline", id: "log-kern-l" }, h("input", { type: "checkbox", id: "log-kern" }), "ядро"),
      h("input", { id: "log-q", type: "search", placeholder: "Фильтр…" }),
      h("label", { class: "inline", id: "log-live-l" }, h("input", { type: "checkbox", id: "log-live", checked: true }), "живое"),
      h("button", { class: "btn", id: "log-reload" }, "Обновить"),
      h("button", { class: "btn hidden", id: "log-ev-save" }, "Скачать"),
      h("button", { class: "btn danger", id: "log-clear" }, "Очистить"),
    ),
    diagCard(),
    h("div", { class: "card", id: "log-body" }, "Загрузка…"),
  );
  const $ = (id) => el.querySelector("#" + id);
  // (a collection may be absent)
  const services = (await Promise.all(["/redfish/v1/Managers/bmc/LogServices", "/redfish/v1/Systems/system/LogServices"].map((b) => api.collection(b).catch(() => [])))).flat();
  if (!root) return;
  $("log-svc").append(...services.map((s) => h("option", { value: s["@odata.id"] }, `Redfish: ${s.Name || s.Id}`)));
  const q = () => $("log-q").value.toLowerCase();
  const reload = () => {
    const v = $("log-svc").value;
    const bmc = v === BMC;
    for (const id of ["log-kern-l", "log-live-l"]) $(id).classList.toggle("hidden", !bmc);
    $("log-prio").classList.toggle("hidden", !bmc && v !== EV);
    $("log-clear").classList.toggle("hidden", bmc || v === EV);
    $("log-ev-save").classList.toggle("hidden", v !== EV);
    if (v === EV) evLoad();
    else if (bmc) jReload();
    else load($("log-svc").value, q());
  };
  const refilter = () => {
    const v = $("log-svc").value;
    if (v === EV) evRender();
    else if (v === BMC) jRender();
    else load(v, q());
  };
  $("log-ev-save").addEventListener("click", () => saveBlob(new Blob([EVT.text], { type: "text/plain" }), "reimu-events.log"));
  $("log-svc").addEventListener("change", reload);
  $("log-q").addEventListener("input", refilter);
  $("log-prio").addEventListener("change", refilter);
  $("log-kern").addEventListener("change", refilter);
  $("log-reload").addEventListener("click", () => {
    cache.url = null;
    reload();
  });
  $("log-live").addEventListener("change", () => $("log-live").checked && jPoll());
  $("log-clear").addEventListener("click", async () => {
    const svc = services.find((s) => s["@odata.id"] === $("log-svc").value);
    const act = svc && svc.Actions && svc.Actions["#LogService.ClearLog"];
    if (!act) return toast("Эта служба журнала не поддерживает очистку", "err");
    if (!confirm("Очистить журнал?")) return;
    try {
      await api.post(act.target, {});
      toast("Журнал очищен", "ok");
      cache.url = null;
      reload();
    } catch (e) {
      toast(e.message, "err");
    }
  });
  reload();
  timer = setInterval(() => {
    if (root && $("log-svc").value === BMC && $("log-live").checked && !document.hidden) jPoll();
  }, 3000);
}

// ---------------------------------------------------------------- downloads
function saveBlob(blob, name) {
  const a = h("a", { href: URL.createObjectURL(blob), download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}

function fromB64(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// the chunks of Read or DiagDownload, in order
async function fetchChunks(method, size, onProgress) {
  const parts = [];
  let off = 0;
  for (;;) {
    const r = await logsCall(method, [off, size]);
    if (!r.ok) throw new Error(r.error || "ошибка чтения");
    const d = fromB64(r.data || "");
    parts.push(d);
    off += d.length;
    if (onProgress) onProgress(off, r.size);
    if (!d.length || off >= r.size) return parts;
  }
}

// ---------------------------------------------------------------- the event log
// /var/lib/reimu-logd/events.log (reimu-logd): warnings and worse, boot records

let EVT = { text: "", error: null };

async function evLoad() {
  const body = root && root.querySelector("#log-body");
  if (body) body.textContent = "Загрузка…";
  try {
    const parts = await fetchChunks("Read", 131072);
    EVT = { text: await new Blob(parts).text(), error: null };
  } catch (e) {
    EVT = { text: "", error: e.status === 403 || e.status === 401 ? "нужны права администратора" : e.status === 404 ? "сервис reimu-logd не отвечает" : e.message };
  }
  evRender();
}

function evRender() {
  const body = root && root.querySelector("#log-body");
  if (!body || root.querySelector("#log-svc").value !== EV) return;
  if (EVT.error) return body.replaceChildren(h("div", { class: "empty" }, `События BMC: ${EVT.error}`));
  const q = root.querySelector("#log-q").value.toLowerCase();
  const maxp = Number(root.querySelector("#log-prio").value);
  const rows = [];
  const lines = EVT.text.split("\n");
  for (let i = lines.length - 1; i >= 0; i--) { // all of it: the file is at most 512 KiB
    const m = /^(\S+) (\d) ([^:]*): (.*)$/.exec(lines[i]);
    if (!m) continue;
    const p = Number(m[2]);
    if (p > maxp || (q && !lines[i].toLowerCase().includes(q))) continue;
    rows.push(h("div", { class: `jl p${p}` }, h("span", { class: "jl-t" }, m[1].replace("T", " ").replace("Z", "")), h("span", { class: "jl-p" }, PRIO[p]), h("span", { class: "jl-s" }, m[3]), h("span", { class: "jl-m" }, m[4])));
  }
  body.replaceChildren(rows.length ? h("div", { class: "jlog", id: "ev-rows" }, rows) : h("div", { class: "empty" }, "Записей нет"));
}

// ---------------------------------------------------------------- diagnostic data
function diagCard() {
  const note = h("div", { class: "muted small", id: "diag-state" }, "");
  const hostBox = h("input", { type: "checkbox", id: "diag-host" });
  const btn = h("button", { class: "btn", id: "diag-go" }, "Скачать диагностические данные");
  const say = (t) => (note.textContent = t);
  btn.addEventListener("click", async () => {
    btn.disabled = true;
    try {
      say("Сбор данных…");
      await logsCall("DiagStart", [hostBox.checked]);
      for (;;) {
        await new Promise((r) => setTimeout(r, 1500));
        if (!root) return;
        const st = await logsCall("DiagStatus", []);
        if (st.state === "running") say(`Сбор данных: ${st.done || 0} из ${st.total || "…"} (${st.step || ""})`);
        else if (st.state === "failed") throw new Error(st.error || "сбор не удался");
        else if (st.state === "done") {
          const parts = await fetchChunks("DiagDownload", 524288, (o, n) => say(`Загрузка: ${Math.round((100 * o) / n)} %`));
          saveBlob(new Blob(parts, { type: "application/gzip" }), st.name);
          await logsCall("DiagDelete", []);
          say(`Готово: ${st.name}. Отправьте этот файл разработчикам.`);
          break;
        } else throw new Error("архив не найден");
      }
    } catch (e) {
      say(e.status === 403 || e.status === 401 ? "Скачать диагностические данные может только администратор" : `Ошибка: ${e.message}`);
    }
    btn.disabled = false;
  });
  return h(
    "div",
    { class: "card section", id: "diag-card" },
    h("h3", {}, "Диагностические данные"),
    h("p", { class: "small" }, "В архиве есть серийные номера, MAC и IP; паролей и ключей нет."),
    h(
      "details",
      { class: "small muted" },
      h("summary", {}, "Что в архиве"),
      h("p", {}, "Версии прошивки и состояние слотов A/B; журнал BMC, журнал событий, сообщения ядра; причина последнего запуска; службы и память; настройки сети; состояние USB; вентиляторы, перегрев, датчики, состояние хоста и связи с ним; FRU. На шинах I2C ничего дополнительно не читается, службы не перезапускаются. Архив хранится на BMC до скачивания, не дольше 15 минут."),
    ),
    h("div", { class: "row" }, h("label", { class: "inline" }, hostBox, "Добавить данные хоста (через агент: оборудование, журнал ядра, EDAC, GPU; файлов пользователя нет)")),
    h("div", { class: "row" }, btn),
    note,
  );
}

// ---------------------------------------------------------------- BMC journal
// journald of the BMC, read by reimu-pnsd (the stock bmcweb is built
// without its journal log service). Newest entries first.

const PAGE = 1000;
const KEEP = 20000;
const PRIO = ["emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"];
let J = { entries: [], first: null, last: null, older: false, busy: false, error: null };

async function jRead(cursor, count) {
  const r = await api.post("/xyz/openbmc_project/reimu/journal/action/Read", { data: [cursor || "", count] });
  return JSON.parse(r.data);
}

async function jReload() {
  J = { entries: [], first: null, last: null, older: false, busy: true, error: null };
  const body = root && root.querySelector("#log-body");
  if (body) body.textContent = "Загрузка…";
  try {
    const r = await jRead("", -PAGE);
    J.entries = r.entries || [];
    J.first = r.first || null;
    J.last = r.last || null;
    J.older = !!r.older;
    if (r.error) J.error = "журнал недоступен";
  } catch (e) {
    J.error = e.status === 404 ? "сервис reimu-pnsd не отвечает или устарел (нужна новая версия devkit)" : e.message;
  }
  J.busy = false;
  jRender();
}

async function jPoll() {
  if (J.busy || !J.last) return;
  J.busy = true;
  try {
    for (let i = 0; i < 5; i++) {
      const r = await jRead(J.last, PAGE);
      if (!r.entries || !r.entries.length) break;
      J.entries.push(...r.entries);
      J.last = r.last;
      if (J.entries.length > KEEP) {
        J.entries.splice(0, J.entries.length - KEEP);
        J.older = true;
        J.first = null;
      }
      jRender();
      if (r.entries.length < PAGE) break;
    }
  } catch (e) {
    /* next poll retries */
  }
  J.busy = false;
}

async function jOlder() {
  if (J.busy || !J.first) return;
  J.busy = true;
  try {
    const r = await jRead(J.first, -PAGE);
    J.entries.unshift(...(r.entries || []));
    if (r.first) J.first = r.first;
    J.older = !!r.older && !!r.first;
  } catch (e) {
    toast(e.message, "err");
  }
  J.busy = false;
  jRender();
}

const pad = (n, w = 2) => String(n).padStart(w, "0");
function stamp(us) {
  const d = new Date(us / 1000);
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
}

function jRender() {
  const body = root && root.querySelector("#log-body");
  if (!body || root.querySelector("#log-svc").value !== BMC) return;
  if (J.error) return body.replaceChildren(h("div", { class: "empty" }, `Журнал BMC: ${J.error}`));
  const q = root.querySelector("#log-q").value.toLowerCase();
  const maxp = Number(root.querySelector("#log-prio").value);
  const kern = root.querySelector("#log-kern").checked;
  const rows = [];
  for (let i = J.entries.length - 1; i >= 0 && rows.length < 3000; i--) {
    const e = J.entries[i];
    const p = e.p === undefined ? 6 : e.p;
    if (p > maxp || (kern && !e.k)) continue;
    const src = `${e.k ? "kernel" : e.id || "?"}${e.pid && !e.k ? `[${e.pid}]` : ""}`;
    if (q && !`${src} ${e.m || ""}`.toLowerCase().includes(q)) continue;
    rows.push(
      h(
        "div",
        { class: `jl p${p}` },
        h("span", { class: "jl-t" }, stamp(e.t)),
        h("span", { class: "jl-p" }, PRIO[p]),
        h("span", { class: "jl-s" }, src),
        h("span", { class: "jl-m" }, e.m || ""),
      ),
    );
  }
  const more = J.older
    ? h("button", { class: "btn small", onclick: jOlder }, J.busy ? "Загрузка…" : `Загрузить ещё ${PAGE}`)
    : h("span", { class: "muted small" }, J.entries.length ? "это начало журнала" : "");
  body.replaceChildren(
    h("div", { class: "row small muted jl-head" }, `записей: ${J.entries.length}${rows.length < J.entries.length ? `, показано: ${rows.length}` : ""}`),
    rows.length ? h("div", { class: "jlog" }, rows) : h("div", { class: "empty" }, J.busy ? "Загрузка…" : "Записей нет"),
    h("div", { class: "row end section" }, more),
  );
}

// ---------------------------------------------------------------- Redfish

let cache = { url: null, entries: [] };

async function load(svcUrl, q) {
  const body = root && root.querySelector("#log-body");
  if (!body || !svcUrl) return;
  if (cache.url !== svcUrl) {
    body.textContent = "Загрузка…";
    try {
      const col = await api.get(`${svcUrl}/Entries`);
      cache = { url: svcUrl, entries: col.Members || [] };
    } catch (e) {
      body.textContent = `Ошибка: ${e.message}`;
      return;
    }
  }
  const rows = cache.entries.filter((e) => !q || JSON.stringify(e).toLowerCase().includes(q)).sort((a, b) => String(b.Created || "").localeCompare(String(a.Created || "")));
  body.replaceChildren(
    rows.length
      ? h(
          "table",
          { class: "tbl" },
          h("thead", {}, h("tr", {}, h("th", { class: "m-hide" }, "№"), h("th", {}, "Время"), h("th", {}, "Важность"), h("th", {}, "Сообщение"))),
          h("tbody", {}, rows.slice(0, 2000).map((e) => h("tr", {}, h("td", { class: "muted m-hide" }, e.Id), h("td", { class: "mono small" }, (e.Created || "").replace("T", " ")), h("td", {}, h("span", { class: `badge ${SEV[e.Severity] || ""}` }, e.Severity || "—")), h("td", {}, e.Message || e.Name || "")))),
        )
      : h("div", { class: "empty" }, "Записей нет (события платформы: датчики, питание, ошибки)"),
  );
  if (rows.length > 2000) body.append(h("div", { class: "muted small" }, `Показаны 2000 из ${rows.length}`));
}

export function unmount() {
  clearInterval(timer);
  root = null;
  cache = { url: null, entries: [] };
  J = { entries: [], first: null, last: null, older: false, busy: false, error: null };
}
