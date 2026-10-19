import { api, h, healthBadge } from "../api.js";

let root, timer;
let filter = "";
let showIdle = false;

const num = (v, d = 1) => (v === undefined || v === null ? "—" : Number(v).toFixed(d));

// Platform sensors read by reimu-pnsd from the ПНС device tree's devices
// (Redfish has none on boards whose device tree hwmon-instantiator does not
// understand). Shared with the overview page.
export async function platformSensors() {
  try {
    const r = await api.get("/xyz/openbmc_project/reimu/sensors");
    return JSON.parse(r.data.Json);
  } catch (e) {
    return null;
  }
}

const STATUS = {
  ok: { State: "Enabled", Health: "OK" },
  warn: { State: "Enabled", Health: "Warning" },
  crit: { State: "Enabled", Health: "Critical" },
  na: { State: "нет сигнала" },
};

// reimu-pnsd sensor -> row of the tables below
export function rowOf(s) {
  return { group: s.group, name: s.name, src: s.src, value: s.value, warn: s.warn, crit: s.crit, duty: s.duty, status: STATUS[s.status] || {} };
}

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
  const [chassis, plat] = await Promise.all([api.collection("/redfish/v1/Chassis").catch(() => []), platformSensors()]);
  const data = { temp: [], fan: [], in: [], curr: [], power: [], platform: plat };
  await Promise.all(
    chassis.map(async (c) => {
      const [t, p] = await Promise.all([
        c.Thermal ? api.get(c.Thermal["@odata.id"]).catch(() => null) : null,
        c.Power ? api.get(c.Power["@odata.id"]).catch(() => null) : null,
      ]);
      const tag = (arr, list, key) =>
        (arr || []).forEach((x) =>
          list.push({ group: `Redfish: ${c.Id}`, name: x.Name, value: x[key], warn: x.UpperThresholdNonCritical, crit: x.UpperThresholdCritical, status: x.Status, pct: x.ReadingUnits === "Percent" }),
        );
      if (t) {
        tag(t.Temperatures, data.temp, "ReadingCelsius");
        tag(t.Fans, data.fan, "Reading");
      }
      if (p) tag(p.Voltages, data.in, "ReadingVolts");
    }),
  );
  if (plat && plat.sensors) plat.sensors.forEach((s) => data[s.kind] && data[s.kind].push(rowOf(s)));
  last = data;
  if (root) render(data);
}

function table(title, rows, unit, digits, opts = {}) {
  let rs = rows.filter((r) => !filter || `${r.group} ${r.name}`.toLowerCase().includes(filter));
  const idle = opts.hideIdle ? rs.filter((r) => r.value === null || r.value === undefined) : [];
  if (opts.hideIdle && !showIdle) rs = rs.filter((r) => !idle.includes(r));
  const cmp = (a, b) => a.localeCompare(b, "ru", { numeric: true });
  rs.sort((a, b) => cmp(a.group || "", b.group || "") || cmp(a.name || "", b.name || ""));
  const value = (r) => (r.value === undefined || r.value === null ? "—" : `${num(r.value, digits)} ${r.pct ? "%" : unit}`);
  let more = null;
  if (idle.length) {
    const a = h("a", { href: "#" }, showIdle ? "скрыть каналы без сигнала" : `показать каналы без сигнала (${idle.length})`);
    a.addEventListener("click", (e) => {
      e.preventDefault();
      showIdle = !showIdle;
      render(last);
    });
    more = h("div", { class: "row end small" }, a);
  }
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
            h("tr", {}, h("th", {}, "Устройство"), h("th", {}, "Датчик"), h("th", {}, "Значение"), opts.duty ? h("th", {}, "Управление") : null, h("th", {}, "Состояние"), h("th", {}, "Предупр."), h("th", {}, "Авария"), h("th", {}, "Адрес")),
          ),
          h(
            "tbody",
            {},
            rs.map((r) =>
              h(
                "tr",
                {},
                h("td", {}, r.group),
                h("td", {}, r.name),
                h("td", { class: "mono" }, value(r)),
                opts.duty ? h("td", { class: "mono" }, r.duty === undefined || r.duty === null ? "—" : `${num(r.duty, 0)} %`) : null,
                h("td", {}, healthBadge(r.status)),
                h("td", { class: "mono muted" }, num(r.warn, digits)),
                h("td", { class: "mono muted" }, num(r.crit, digits)),
                h("td", { class: "mono muted small" }, r.src || ""),
              ),
            ),
          ),
        )
      : h("div", { class: "empty" }, idle.length ? "Все каналы без сигнала" : "Нет датчиков"),
    more,
  );
}

function platformNote(p) {
  if (!p) return h("div", { class: "note warn section" }, "Сервис reimu-pnsd недоступен: показаны только датчики из Redfish.");
  if (p.platform === "off") return h("div", { class: "note section" }, "Платформа выключена: её датчики питаются от неё и читаются только при включённом питании.");
  if (p.platform === "starting") return h("div", { class: "note section" }, "Платформа включается: датчики начнут читаться через ~30 с после включения, чтобы не мешать её запуску на шине I²C.");
  if (p.error) return h("div", { class: "note warn section" }, `Датчики платформы не прочитаны: ${p.error === "no device tree" ? "нет devtree ПНС (/var/volatile/motherboard_devtree.dtb)" : p.error}`);
  return null;
}

function render(d) {
  if (!d || !root) return;
  root
    .querySelector("#sens-body")
    .replaceChildren(
      ...[
        platformNote(d.platform),
        table("Температуры", d.temp, "°C", 1),
        table("Вентиляторы", d.fan, "об/мин", 0, { duty: true, hideIdle: true }),
        table("Напряжения", d.in, "В", 3),
        table("Токи", d.curr, "А", 2),
        table("Мощность", d.power, "Вт", 2),
      ].filter(Boolean),
    );
}

export function unmount() {
  clearInterval(timer);
  root = null;
}
