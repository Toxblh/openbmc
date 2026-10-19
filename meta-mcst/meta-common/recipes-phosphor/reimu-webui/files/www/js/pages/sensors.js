import { api, h, healthBadge } from "../api.js";

let root, timer;
let filter = "";

const num = (v, d = 1) => (v === undefined || v === null ? "—" : Number(v).toFixed(d));

export async function mount(el) {
  root = el;
  const search = h("input", { type: "search", placeholder: "Фильтр по имени…", size: 24 });
  const auto = h("input", { type: "checkbox", checked: true });
  search.addEventListener("input", () => {
    filter = search.value.toLowerCase();
    render(last);
  });
  auto.addEventListener("change", () => {
    clearInterval(timer);
    if (auto.checked) timer = setInterval(refresh, 5000);
  });
  el.append(
    h("div", { class: "row section" }, h("h2", {}, "Датчики"), h("span", { class: "spacer" }), search, h("label", { class: "inline" }, auto, "автообновление 5 с")),
    h("div", { id: "sens-body" }, h("div", { class: "empty" }, "Загрузка…")),
  );
  await refresh();
  timer = setInterval(refresh, 5000);
}

let last = null;

async function refresh() {
  const chassis = await api.collection("/redfish/v1/Chassis").catch(() => []);
  const data = { temps: [], fans: [], volts: [] };
  await Promise.all(
    chassis.map(async (c) => {
      const [t, p] = await Promise.all([
        c.Thermal ? api.get(c.Thermal["@odata.id"]).catch(() => null) : null,
        c.Power ? api.get(c.Power["@odata.id"]).catch(() => null) : null,
      ]);
      const tag = (arr, list) => (arr || []).forEach((x) => list.push({ ...x, chassis: c.Id }));
      if (t) {
        tag(t.Temperatures, data.temps);
        tag(t.Fans, data.fans);
      }
      if (p) tag(p.Voltages, data.volts);
    }),
  );
  last = data;
  if (root) render(data);
}

function thr(x, unitKey) {
  const lc = x.LowerThresholdCritical,
    lw = x.LowerThresholdNonCritical,
    uw = x.UpperThresholdNonCritical,
    uc = x.UpperThresholdCritical;
  return [lc, lw, uw, uc].map((v) => h("td", { class: "mono muted" }, num(v, unitKey === "rpm" ? 0 : 2)));
}

function table(title, rows, valueKey, unit, digits) {
  const rs = rows.filter((r) => !filter || (r.Name || "").toLowerCase().includes(filter));
  rs.sort((a, b) => (a.Name || "").localeCompare(b.Name || "", "ru", { numeric: true }));
  return h(
    "div",
    { class: "card section" },
    h("h3", {}, title, h("span", { class: "muted small" }, `${rs.length}`)),
    rs.length
      ? h(
          "table",
          { class: "tbl" },
          h(
            "thead",
            {},
            h("tr", {}, h("th", {}, "Имя"), h("th", {}, "Значение"), h("th", {}, "Состояние"), h("th", {}, "Авар. ниж."), h("th", {}, "Пред. ниж."), h("th", {}, "Пред. верх."), h("th", {}, "Авар. верх."), h("th", {}, "Шасси")),
          ),
          h(
            "tbody",
            {},
            rs.map((r) =>
              h(
                "tr",
                {},
                h("td", {}, r.Name),
                h("td", { class: "mono" }, r[valueKey] === undefined || r[valueKey] === null ? "—" : `${num(r[valueKey], digits)} ${r.ReadingUnits === "Percent" ? "%" : unit}`),
                h("td", {}, healthBadge(r.Status)),
                ...thr(r, unit === "об/мин" ? "rpm" : ""),
                h("td", { class: "muted small" }, r.chassis),
              ),
            ),
          ),
        )
      : h("div", { class: "empty" }, "Нет датчиков (часть датчиков платформы появляется только при включённом питании)"),
  );
}

function render(d) {
  if (!d || !root) return;
  root.querySelector("#sens-body").replaceChildren(table("Температуры", d.temps, "ReadingCelsius", "°C", 1), table("Вентиляторы", d.fans, "Reading", "об/мин", 0), table("Напряжения", d.volts, "ReadingVolts", "В", 3));
}

export function unmount() {
  clearInterval(timer);
  root = null;
}
