// Hardware inventory (Redfish) and host boot firmware (ПНС) information
// from reimu-pnsd (D-Bus object /xyz/openbmc_project/reimu/pns).

import { api, h, toast, healthBadge, fmtBytes } from "../api.js";

let root;

const kv = (pairs) => h("dl", { class: "kv" }, pairs.filter(([, v]) => v !== undefined && v !== null && v !== "").flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]));
const hex = (n) => "0x" + Number(n).toString(16).padStart(8, "0");

export async function mount(el) {
  root = el;
  el.append(h("h2", {}, "Оборудование"), h("div", { id: "hw-inv", class: "grid section" }, h("div", { class: "card" }, "Загрузка…")), h("h2", {}, "Программа начального старта (ПНС)"), h("div", { id: "hw-pns" }, h("div", { class: "card" }, "Загрузка…")));
  await Promise.all([loadInventory(), loadPns()]);
}

async function loadInventory() {
  const [sys, chassis, mgr] = await Promise.all([api.get("/redfish/v1/Systems/system").catch(() => null), api.collection("/redfish/v1/Chassis").catch(() => []), api.get("/redfish/v1/Managers/bmc").catch(() => null)]);
  const [cpus, mems] = await Promise.all([
    sys && sys.Processors ? api.collection(sys.Processors["@odata.id"]).catch(() => []) : [],
    sys && sys.Memory ? api.collection(sys.Memory["@odata.id"]).catch(() => []) : [],
  ]);
  if (!root) return;
  const cards = [];
  if (sys)
    cards.push(
      h(
        "div",
        { class: "card" },
        h("h3", {}, "Платформа", healthBadge(sys.Status)),
        kv([
          ["Модель", sys.Model],
          ["Производитель", sys.Manufacturer],
          ["Серийный номер", sys.SerialNumber],
          ["Партномер", sys.PartNumber],
          ["Версия ПНС", sys.BiosVersion],
          ["UUID", sys.UUID],
          ["Питание", { On: "включена", Off: "выключена" }[sys.PowerState] || sys.PowerState],
          ["Память всего", sys.MemorySummary && sys.MemorySummary.TotalSystemMemoryGiB ? `${sys.MemorySummary.TotalSystemMemoryGiB} ГиБ` : undefined],
        ]),
      ),
    );
  for (const c of chassis)
    cards.push(
      h(
        "div",
        { class: "card" },
        h("h3", {}, `Шасси ${c.Name || c.Id}`, healthBadge(c.Status)),
        kv([
          ["Тип", c.ChassisType],
          ["Модель", c.Model],
          ["Производитель", c.Manufacturer],
          ["Серийный номер", c.SerialNumber],
          ["Партномер", c.PartNumber],
          ["Индикатор", c.IndicatorLED],
        ]),
      ),
    );
  if (cpus.length)
    cards.push(
      h(
        "div",
        { class: "card" },
        h("h3", {}, `Процессоры: ${cpus.length}`),
        h("table", { class: "tbl" }, h("tbody", {}, cpus.map((p) => h("tr", {}, h("td", {}, p.Name || p.Id), h("td", {}, p.Model || p.Manufacturer || ""), h("td", {}, p.TotalCores ? `${p.TotalCores} ядер` : ""), h("td", {}, healthBadge(p.Status)))))),
      ),
    );
  if (mems.length)
    cards.push(
      h(
        "div",
        { class: "card" },
        h("h3", {}, `Модули памяти: ${mems.length}`),
        h("table", { class: "tbl" }, h("tbody", {}, mems.map((m) => h("tr", {}, h("td", {}, m.Name || m.Id), h("td", {}, m.CapacityMiB ? fmtBytes(m.CapacityMiB * 1048576) : ""), h("td", {}, m.MemoryDeviceType || ""), h("td", {}, healthBadge(m.Status)))))),
      ),
    );
  if (mgr)
    cards.push(
      h(
        "div",
        { class: "card" },
        h("h3", {}, "BMC", healthBadge(mgr.Status)),
        kv([
          ["Модель", mgr.Model],
          ["Прошивка", mgr.FirmwareVersion],
          ["Тип", mgr.ManagerType],
          ["UUID", mgr.UUID],
        ]),
      ),
    );
  root.querySelector("#hw-inv").replaceChildren(...(cards.length ? cards : [h("div", { class: "card empty" }, "Нет данных инвентаря")]));
}

function parseFru(xml) {
  if (!xml) return [];
  const doc = new DOMParser().parseFromString(xml, "application/xml");
  if (doc.querySelector("parsererror")) return [{ title: "FRU", rows: [["Ошибка", "не удалось разобрать XML"]] }];
  const sections = [];
  for (const area of doc.documentElement.children) {
    const rows = [];
    for (const f of area.children) {
      if (f.getAttribute("type") === "timestamp") continue;
      rows.push([f.tagName.replace(/_/g, " "), f.textContent]);
    }
    sections.push({ title: area.tagName.replace(/_/g, " "), rows });
  }
  return sections;
}

function dtsSummary(dts) {
  if (!dts) return [];
  const get = (re) => {
    const m = dts.match(re);
    return m ? m[1] : undefined;
  };
  return [
    ["Модель (model)", get(/^\s*model\s*=\s*"([^"]*)"/m)],
    ["Совместимость", get(/^\s*compatible\s*=\s*([^;]*);/m)],
    ["Узлов i2c-устройств", (dts.match(/@[0-9a-f]+\s*\{/g) || []).length || undefined],
  ];
}

async function loadPns() {
  const box = root.querySelector("#hw-pns");
  let doc;
  try {
    const r = await api.get("/xyz/openbmc_project/reimu/pns");
    doc = JSON.parse(r.data.Json);
  } catch (e) {
    box.replaceChildren(h("div", { class: "card" }, h("div", { class: "muted" }, `Сервис reimu-pnsd недоступен: ${e.message}`)));
    return;
  }
  if (!root) return;
  const fru = parseFru(doc.fruXml);
  const scan = doc.scan;
  const rescan = h("button", { class: "btn" }, "Перечитать флеш ПНС");
  rescan.addEventListener("click", async () => {
    if (!confirm("Перечитать флеш ПНС? Платформа должна быть выключена: на время чтения флеш отключается от хоста.")) return;
    rescan.disabled = true;
    try {
      const r = await api.post("/xyz/openbmc_project/reimu/pns/action/Rescan", { data: [] });
      const { ok, message } = JSON.parse(r.data);
      toast(message, ok ? "ok" : "err");
      if (ok) await loadPns();
    } catch (e) {
      toast(`Ошибка: ${e.message}`, "err");
    } finally {
      rescan.disabled = false;
    }
  });

  const dtsPre = h("pre", { class: "mono small hidden", style: { maxHeight: "400px", overflow: "auto", background: "var(--bg)", padding: "8px" } }, doc.devtree || "");
  const dtsBtn = h("button", { class: "btn small" }, "Показать device tree");
  dtsBtn.addEventListener("click", () => dtsPre.classList.toggle("hidden"));

  let flash;
  if (!scan) flash = h("div", { class: "muted" }, "Флеш ещё не сканировался из веб-интерфейса.");
  else if (!scan.ok) flash = h("div", { class: "error-text" }, scan.error);
  else {
    const pt = scan.partitionTable;
    flash = h(
      "div",
      {},
      kv([
        ["Объём флеша", fmtBytes(scan.size)],
        ["Прочитан", new Date(scan.time * 1000).toLocaleString()],
        ["Таблица разделов", pt.present ? `версия ${pt.version}, ${pt.entries} записей @ ${hex(pt.offset)}` : `не найдена (ожидалась @ ${hex(pt.offset)})`],
        ["Device tree", scan.dtb.valid ? `${hex(scan.dtb.address)}, ${fmtBytes(scan.dtb.size)}` : `не найден: ${scan.dtb.error || "неверный размер"}`],
      ]),
      pt.present
        ? h(
            "table",
            { class: "tbl" },
            h("thead", {}, h("tr", {}, h("th", {}, "Сигнатура"), h("th", {}, "Назначение"), h("th", {}, "Смещение"), h("th", {}, "Размер"), h("th", {}, "Контрольная сумма"))),
            h(
              "tbody",
              {},
              pt.partitions.map((p) =>
                h("tr", {}, h("td", { class: "mono" }, p.signature), h("td", {}, p.name || h("span", { class: "muted" }, "неизвестно")), h("td", { class: "mono" }, hex(p.offset)), h("td", {}, fmtBytes(p.size), p.inFlash ? "" : h("span", { class: "badge err" }, "за пределами")), h("td", { class: "mono small muted" }, p.checksum.slice(0, 16) + "…")),
              ),
            ),
          )
        : null,
    );
  }

  box.replaceChildren(
    h(
      "div",
      { class: "grid" },
      ...fru.map((s) =>
        h(
          "div",
          { class: "card" },
          h("h3", {}, `FRU: ${s.title}`),
          kv(s.rows),
          doc.fruSource === "eeprom" || doc.fruSource === "cache"
            ? h(
                "div",
                { class: "muted small" },
                doc.fruSource === "cache"
                  ? "сохранённая копия: EEPROM FRU отвечает только при включённой платформе"
                  : "прочитан из EEPROM: при загрузке BMC mcst-fruid его не получил",
              )
            : null,
        ),
      ),
      fru.length ? null : h("div", { class: "card" }, h("h3", {}, "FRU"), h("div", { class: "muted" }, "FRU не прочитан: EEPROM отвечает только при включённой платформе")),
      h("div", { class: "card" }, h("h3", {}, "Device tree платформы"), doc.devtree ? kv(dtsSummary(doc.devtree)) : h("div", { class: "muted" }, "нет данных"), h("div", { class: "row" }, doc.devtree ? dtsBtn : null)),
      h("div", { class: "card span2" }, h("h3", {}, "Флеш ПНС"), flash, h("br"), h("div", { class: "row" }, rescan)),
    ),
    dtsPre,
    h(
      "p",
      { class: "note" },
      "Настройки, которые ПНС сохраняет командой «save params to NVRAM», хранятся в NVRAM часов реального времени платформы. ",
      "Эта микросхема подключена только к хосту, у BMC к ней нет доступа, поэтому из веб-интерфейса доступно только чтение флеша ПНС: FRU, device tree и таблица разделов. ",
      "Изменить загрузку можно через консоль платформы (вкладка «Консоль») в меню ПНС или в /boot/boot.conf.",
    ),
  );
}

export function unmount() {
  root = null;
}
