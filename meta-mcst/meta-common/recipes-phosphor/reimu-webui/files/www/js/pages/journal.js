import { api, h, toast } from "../api.js";

let root, timer;

const SEV = { OK: "ok", Warning: "warn", Critical: "err" };
const BMC = "bmc-journal";

export async function mount(el) {
  root = el;
  el.append(
    h(
      "div",
      { class: "row section log-tools" },
      h("h2", {}, "Журнал"),
      h("span", { class: "spacer" }),
      h("select", { id: "log-svc" }, h("option", { value: BMC }, "BMC (системный журнал)")),
      h("select", { id: "log-prio", title: "Важность" }, h("option", { value: "7" }, "Все"), h("option", { value: "5" }, "Важные"), h("option", { value: "4" }, "Предупреждения"), h("option", { value: "3" }, "Ошибки")),
      h("label", { class: "inline", id: "log-kern-l" }, h("input", { type: "checkbox", id: "log-kern" }), "ядро"),
      h("input", { id: "log-q", type: "search", placeholder: "Фильтр…" }),
      h("label", { class: "inline", id: "log-live-l" }, h("input", { type: "checkbox", id: "log-live", checked: true }), "живое"),
      h("button", { class: "btn", id: "log-reload" }, "Обновить"),
      h("button", { class: "btn danger", id: "log-clear" }, "Очистить"),
    ),
    h("div", { class: "card", id: "log-body" }, "Загрузка…"),
  );
  const $ = (id) => el.querySelector("#" + id);
  const services = [];
  for (const base of ["/redfish/v1/Managers/bmc/LogServices", "/redfish/v1/Systems/system/LogServices"]) {
    try {
      for (const s of await api.collection(base)) services.push(s);
    } catch (e) {
      /* collection may be absent */
    }
  }
  if (!root) return;
  $("log-svc").append(...services.map((s) => h("option", { value: s["@odata.id"] }, `Redfish: ${s.Name || s.Id}`)));
  const q = () => $("log-q").value.toLowerCase();
  const reload = () => {
    const bmc = $("log-svc").value === BMC;
    for (const id of ["log-prio", "log-kern-l", "log-live-l"]) $(id).classList.toggle("hidden", !bmc);
    $("log-clear").classList.toggle("hidden", bmc);
    if (bmc) jReload();
    else load($("log-svc").value, q());
  };
  const refilter = () => ($("log-svc").value === BMC ? jRender() : load($("log-svc").value, q()));
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
