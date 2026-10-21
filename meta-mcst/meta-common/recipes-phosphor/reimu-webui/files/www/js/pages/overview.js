import { api, h, healthBadge } from "../api.js";
import { powerAction } from "../app.js";
import { platformSensors, rowOf } from "./sensors.js";

let root, timer;

const LED = { OK: "on", Warning: "warn", Critical: "err" };

const kv = (pairs) => h("dl", { class: "kv" }, pairs.filter(([, v]) => v !== undefined && v !== null && v !== "").flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]));

export async function mount(el) {
  root = el;
  el.append(h("div", { class: "grid", id: "ov-grid" }, h("div", { class: "card" }, "Загрузка…")));
  await refresh();
  timer = setInterval(refresh, 10000);
}

async function refresh() {
  const [sys, mgr, chassis, plat] = await Promise.all([
    api.get("/redfish/v1/Systems/system").catch(() => null),
    api.get("/redfish/v1/Managers/bmc").catch(() => null),
    api.collection("/redfish/v1/Chassis").catch(() => []),
    platformSensors(),
  ]);
  if (!root) return;
  const temps = [];
  await Promise.all(
    chassis.map(async (c) => {
      if (!c.Thermal) return;
      const t = await api.get(c.Thermal["@odata.id"]).catch(() => null);
      if (t && t.Temperatures) temps.push(...t.Temperatures);
    }),
  );
  if (plat && plat.sensors)
    plat.sensors
      .filter((s) => s.kind === "temp" && s.value !== null)
      .forEach((s) => {
        const r = rowOf(s);
        // "температура" of a regulator says nothing: its device does
        if (r.name === "температура") temps.push({ Name: r.group, ReadingCelsius: r.value, Status: r.status });
        else temps.push({ Name: r.name, Group: r.group, ReadingCelsius: r.value, Status: r.status });
      });
  temps.sort((a, b) => (b.ReadingCelsius || 0) - (a.ReadingCelsius || 0));

  const powerBtns = h(
    "div",
    { class: "row" },
    ...[
      ["On", "Включить", "primary"],
      ["GracefulShutdown", "Выключить", ""],
      ["ForceRestart", "Сброс", "danger"],
    ].map(([k, t, c]) => {
      const b = h("button", { class: `btn ${c}` }, t);
      b.addEventListener("click", () => powerAction(k));
      return b;
    }),
  );
  const on = sys && sys.PowerState === "On";
  const cards = [
    h(
      "div",
      { class: "card" },
      h("h3", {}, "Платформа", sys ? healthBadge(sys.Status) : null),
      sys
        ? kv([
            ["Питание", h("span", {}, h("span", { class: `led ${on ? "on" : "off"}` }), " ", on ? "включена" : sys.PowerState === "Off" ? "выключена" : sys.PowerState)],
            ["Модель", sys.Model],
            ["Производитель", sys.Manufacturer],
            ["Серийный номер", sys.SerialNumber],
            ["Версия ПНС", sys.BiosVersion],
            ["Процессоры", sys.ProcessorSummary && sys.ProcessorSummary.Count ? `${sys.ProcessorSummary.Count} × ${sys.ProcessorSummary.Model || ""}` : undefined],
            ["Индикатор UID", { Off: "выключен", Lit: "горит", Blinking: "мигает" }[sys.IndicatorLED] || sys.IndicatorLED],
          ])
        : h("div", { class: "muted" }, "нет данных"),
      h("br"),
      powerBtns,
    ),
    h(
      "div",
      { class: "card" },
      h("h3", {}, "BMC", mgr ? healthBadge(mgr.Status) : null),
      mgr
        ? kv([
            ["Прошивка", mgr.FirmwareVersion],
            ["Модель", mgr.Model],
            ["Время BMC", mgr.DateTime ? mgr.DateTime.replace("T", " ").replace(/([+-]\d\d:\d\d|Z)$/, (z) => (z === "+00:00" || z === "Z" ? " UTC" : ` ${z}`)) : undefined],
            ["Время браузера", new Date().toLocaleString()],
            ["UUID", mgr.UUID],
          ])
        : h("div", { class: "muted" }, "нет данных"),
    ),
    h(
      "div",
      { class: "card" },
      h("h3", {}, "Температуры"),
      temps.length
        ? h(
            "div",
            { class: "temps" },
            temps.slice(0, 8).map((t) =>
              h(
                "div",
                { class: "temp-row", title: t.Status && t.Status.Health ? `Состояние: ${t.Status.Health}` : "" },
                h("span", { class: `led ${LED[t.Status && t.Status.Health] || ""}` }),
                h("span", { class: "temp-name" }, t.Name, t.Group ? h("span", { class: "muted small" }, t.Group) : null),
                h("span", { class: "temp-val mono" }, t.ReadingCelsius !== undefined && t.ReadingCelsius !== null ? `${Number(t.ReadingCelsius).toFixed(1)}\u00a0°C` : "—"),
              ),
            ),
          )
        : h("div", { class: "muted" }, plat && plat.platform === "starting" ? "платформа включается: датчики появятся через ~30 с" : on ? "датчики не найдены" : "платформа выключена: датчики платформы появятся после включения"),
      h("div", { class: "row end" }, h("a", { href: "#/sensors" }, "все датчики →")),
    ),
    h(
      "div",
      { class: "card" },
      h("h3", {}, "Быстрый доступ"),
      h(
        "div",
        { class: "row" },
        h("a", { class: "btn", href: "#/kvm" }, "Экран (KVM)"),
        h("a", { class: "btn", href: "#/console" }, "Консоль COM"),
        h("a", { class: "btn", href: "#/media" }, "Виртуальный носитель"),
        h("a", { class: "btn", href: "#/hardware" }, "Оборудование и ПНС"),
      ),
    ),
  ];
  root.querySelector("#ov-grid").replaceChildren(...cards);
}

export function unmount() {
  clearInterval(timer);
  root = null;
}
