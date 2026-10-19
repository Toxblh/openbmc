import { api, h, toast } from "../api.js";

let root;

const SEV = { OK: "ok", Warning: "warn", Critical: "err" };

export async function mount(el) {
  root = el;
  el.append(h("div", { class: "row section" }, h("h2", {}, "Журнал событий"), h("span", { class: "spacer" }), h("select", { id: "log-svc" }), h("input", { id: "log-q", type: "search", placeholder: "Фильтр…" }), h("button", { class: "btn", id: "log-reload" }, "Обновить"), h("button", { class: "btn danger", id: "log-clear" }, "Очистить")), h("div", { class: "card", id: "log-body" }, "Загрузка…"));
  const $ = (id) => el.querySelector("#" + id);
  const services = [];
  for (const base of ["/redfish/v1/Systems/system/LogServices", "/redfish/v1/Managers/bmc/LogServices"]) {
    try {
      for (const s of await api.collection(base)) services.push(s);
    } catch (e) {
      /* collection may be absent */
    }
  }
  $("log-svc").replaceChildren(...services.map((s) => h("option", { value: s["@odata.id"] }, s.Name || s.Id)));
  const reload = () => load($("log-svc").value, $("log-q").value.toLowerCase());
  $("log-svc").addEventListener("change", reload);
  $("log-q").addEventListener("input", reload);
  $("log-reload").addEventListener("click", reload);
  $("log-clear").addEventListener("click", async () => {
    const svc = services.find((s) => s["@odata.id"] === $("log-svc").value);
    const act = svc && svc.Actions && svc.Actions["#LogService.ClearLog"];
    if (!act) return toast("Эта служба журнала не поддерживает очистку", "err");
    if (!confirm("Очистить журнал?")) return;
    try {
      await api.post(act.target, {});
      toast("Журнал очищен", "ok");
      reload();
    } catch (e) {
      toast(e.message, "err");
    }
  });
  if (services.length) reload();
  else $("log-body").replaceChildren(h("div", { class: "empty" }, "Службы журналов не найдены"));
}

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
          h("thead", {}, h("tr", {}, h("th", {}, "№"), h("th", {}, "Время"), h("th", {}, "Важность"), h("th", {}, "Сообщение"))),
          h("tbody", {}, rows.slice(0, 2000).map((e) => h("tr", {}, h("td", { class: "muted" }, e.Id), h("td", { class: "mono small" }, (e.Created || "").replace("T", " ")), h("td", {}, h("span", { class: `badge ${SEV[e.Severity] || ""}` }, e.Severity || "—")), h("td", {}, e.Message || e.Name || "")))),
        )
      : h("div", { class: "empty" }, "Записей нет"),
  );
  if (rows.length > 2000) body.append(h("div", { class: "muted small" }, `Показаны 2000 из ${rows.length}`));
}

export function unmount() {
  root = null;
  cache = { url: null, entries: [] };
}
