import { api, h, healthBadge } from "../api.js";
import { fanState, renderFans } from "./fans.js";

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
    h("div", { class: "row section" }, h("h2", { class: "m-hide" }, "Датчики"), h("span", { class: "spacer m-hide" }), search, h("label", { class: "inline" }, auto, "автообновление 5 с")),
    h("div", { id: "fans-body" }),
    h("div", { id: "sens-body" }, h("div", { class: "empty" }, "Загрузка…")),
  );
  await refresh();
  timer = setInterval(refresh, 5000);
}

let last = null;

async function refresh() {
  const [chassis, plat, fans] = await Promise.all([api.collection("/redfish/v1/Chassis").catch(() => []), platformSensors(), fanState()]);
  const data = { temp: [], fan: [], in: [], curr: [], power: [], platform: plat, fans };
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
  rs.sort((a, b) => cmp(a.group || "", b.group || "") || cmp(a.src || "", b.src || "") || cmp(a.name || "", b.name || ""));
  const reading = (r) => (r.value === undefined || r.value === null ? "—" : `${num(r.value, digits)}\u00a0${r.pct ? "%" : unit}`);
  // fans: the speed and, after it, the drive the controller gives the fan
  const value = (r) => (opts.duty && r.duty !== undefined && r.duty !== null ? `${reading(r)} · ${num(r.duty, 0)}\u00a0%` : reading(r));
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
          { class: "tbl sens-tbl" },
          h(
            "thead",
            {},
            h(
              "tr",
              {},
              h("th", { class: "m-hide c-dev" }, "Устройство"),
              h("th", { class: "c-name" }, "Датчик"),
              h("th", { class: "c-val" }, "Значение"),
                            h("th", { class: "c-st" }, "Сост."),
              h("th", { class: "m-hide c-lim" }, "Предупр."),
              h("th", { class: "m-hide c-lim" }, "Авария"),
              h("th", { class: "m-hide" }, "Адрес"),
            ),
          ),
          h(
            "tbody",
            {},
            rs.map((r) =>
              h(
                "tr",
                {},
                h("td", { class: "m-hide" }, r.group),
                // on a phone the device column is hidden: its name goes above the sensor's
                h("td", {}, h("div", { class: "m-only muted small" }, r.group), r.name),
                h("td", { class: "mono" }, value(r)),
                h("td", {}, healthBadge(r.status)),
                h("td", { class: "mono muted m-hide" }, num(r.warn, digits)),
                h("td", { class: "mono muted m-hide" }, num(r.crit, digits)),
                h("td", { class: "mono muted small m-hide" }, r.src || ""),
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
  renderFans(root.querySelector("#fans-body"), d.fans, refresh);
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
