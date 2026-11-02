// Карта платы, часть страницы «Оборудование»: одна на все платы. Здесь данные BMC и агента хоста в
// модель и сводка; что и где стоит в js/boards/*.js, рисование в js/lib/boardscene.js.
// Чего данные не говорят, то показано нейтрально, как неизвестное.
import { api, h, fmtBytes } from "../api.js";
import { createScene, ZOOMS } from "../lib/boardscene.js";

const BOARDS = [{ re: /1E8SV-uATX|1E8CB-uATX|TVGI\.469555\.445/i, load: () => import("../boards/1e8sv-uatx.js") }];
const P = "/xyz/openbmc_project/reimu";
const VIEW_KEY = "reimu-map-view";
const PHONE = 768;
const MIN_STAGE = 320;
const json = (p) => api.get(p).then((r) => JSON.parse(r.data.Json)).catch(() => null);
const AGENT_OLD = "Этот агент не сообщает об этом: его надо обновить.";
const NO_AGENT = "Агент хоста не подключён, данных нет.";

let root, timer, board, fanText, stage, summary, stateBox, scene, model, shape, id;
let view = "persp";

// nothing stored or anything else: the perspective
function loadView() {
  try {
    const v = localStorage.getItem(VIEW_KEY);
    return v === "2d" ? "top" : v === "iso" ? "iso" : "persp";
  } catch (e) {
    return "persp";
  }
}

// the stage takes what the window leaves with the cards under it; a phone goes by its width
function stageHeight() {
  if (!root || !stage || innerWidth < PHONE) return null;
  const sc = root.closest(".page");
  const box = sc ? sc.getBoundingClientRect() : { top: 0 };
  const bottom = sc ? box.top + sc.clientHeight - parseFloat(getComputedStyle(sc).paddingBottom) : innerHeight;
  const top = stage.getBoundingClientRect().top + (sc ? sc.scrollTop : scrollY);
  return Math.max(MIN_STAGE, bottom - top - summary.getBoundingClientRect().height - 14);
}

function zoomBy(dir) {
  if (scene) scene.zoom(dir);
  showZoom();
}

function showZoom() {
  const z = scene ? scene.zoomState().Z : 1;
  root.querySelector("[data-zoom='reset']").textContent = `${Math.round(z * 100)} %`;
  root.querySelector("[data-zoom='out']").disabled = z <= ZOOMS[0];
  root.querySelector("[data-zoom='in']").disabled = z >= ZOOMS[ZOOMS.length - 1];
}

function setView(v) {
  view = v;
  root.querySelectorAll("[data-view-btn]").forEach((b) => b.classList.toggle("primary", b.dataset.viewBtn === v));
  try {
    localStorage.setItem(VIEW_KEY, v === "top" ? "2d" : v);
  } catch (e) {
    /* a remembered view is only a convenience */
  }
  if (scene) scene.setView(v);
}

const fruField = (xml, tag) => {
  const m = new RegExp(`<${tag}>([^<]*)<`).exec(xml || "");
  return m ? m[1] : null;
};

export async function identify() {
  const pns = await json(`${P}/pns`);
  if (!pns) return { name: null };
  const model2 = /^\s*model\s*=\s*"([^"]*)"/m.exec(pns.devtree || "");
  const fru = fruField(pns.fruXml, "product_name");
  return {
    name: (model2 && model2[1]) || fru, fru,
    part: fruField(pns.fruXml, "part_number"),
    serial: fruField(pns.fruXml, "serial_number"),
    vendor: fruField(pns.fruXml, "manufacturer"),
  };
}

// true: the map is there; a page around it goes on whatever happens here
export async function mount(el) {
  root = el;
  scene = null;
  shape = null;
  id = await identify();
  const known = BOARDS.find((b) => [id.name, id.fru, id.part].some((s) => s && b.re.test(s)));
  if (root !== el) return false;
  if (!known) {
    el.append(h("div", { class: "card", id: "map-unknown" }, h("h3", {}, "Карта платы"),
      id.name ? `Для платы «${id.name}» карты пока нет. Датчики есть на странице «Датчики».` : "Плата не определена: нет ни описания платы, ни данных FRU."));
    return false;
  }
  const mod = await known.load();
  board = mod.default;
  fanText = mod.fanText;
  if (root !== el) return false;
  const seg = (v, t) => h("button", { class: "btn small", dataset: { viewBtn: v }, onclick: () => setView(v) }, t);
  const zb = (n, dir, t, title) => h("button", { class: "btn small", dataset: { zoom: n }, title, onclick: () => zoomBy(dir) }, t);
  stage = h("div", { class: "bs-stage" });
  stateBox = h("div", { id: "map-state" });
  summary = h("div", { class: "grid bs-sum", id: "map-summary" });
  el.append(
    h("div", { class: "row section" }, h("h3", { class: "bs-title" }, `Карта платы ${board.name}`), h("span", { class: "spacer" }), zb("out", -1, "−", "Уменьшить"), zb("reset", 0, "100 %", "Вся плата"), zb("in", 1, "+", "Увеличить"), seg("iso", "Изометрия"), seg("persp", "Перспектива"), seg("top", "Сверху")),
    stateBox,
    h("div", { class: "card bs-board" }, stage),
    summary,
    h("p", { class: "muted small" }, `Положения по ${board.doc}. Наведите или нажмите на деталь. Пунктирный маркер: положение датчика приблизительное.`),
  );
  setView(loadView());
  await refresh();
  if (root !== el) return false;
  timer = setTimeout(() => poll(el), 5000);
  return true;
}

// the next cycle only after this one: a slow BMC gets no queue of requests
async function poll(el) {
  try {
    await refresh();
  } finally {
    if (root === el) timer = setTimeout(() => poll(el), 5000);
  }
}

export function unmount() {
  clearTimeout(timer);
  if (scene) scene.destroy();
  scene = null;
  root = null;
}

// ------------------------------------------------------------------ the model
function usbPort(u) {
  const m = /^(\d+)-(\d+)/.exec(u.name || "");
  if (!m) return null;
  const c = board.usb.roots[m[1]];
  return c === undefined ? null : `${c}${Number(m[2]) - 1}`;
}

function usbConnector(u) {
  const p = usbPort(u);
  return p === null ? null : board.usb.ports[p] || null;
}

// from a device up to the root port of a slot
function rootOf(slot, byslot, roots) {
  for (let s = slot, n = 0; s && n < 16; n++) {
    if (roots.includes(s)) return s;
    s = byslot[s] && byslot[s].parent;
  }
  return null;
}

function pcieSlots(on, inv) {
  const pci = inv.pci || [];
  const byslot = Object.fromEntries(pci.map((d) => [d.slot, d]));
  const topo = pci.some((d) => d.parent);
  const out = {};
  for (const [sid, s] of Object.entries(board.pcie.slots)) {
    const base = { slot: s.width, state: "unknown", note: !on ? NO_AGENT : !topo ? AGENT_OLD : s.inferred ? "Порт этого разъёма определён косвенно: на этих портах нет ничего встроенного." : "" };
    if (!on || !topo) {
      out[sid] = base;
      continue;
    }
    // the device: the first that isn't a bridge or an audio function; the mode: at the
    // root port, the device's own link is the inside of the card (a GPU's switch says x16)
    const devs = pci.filter((d) => !String(d.class).startsWith("0604") && !String(d.class).startsWith("04") && rootOf(d.slot, byslot, s.roots));
    const d = devs[0];
    if (!d) {
      out[sid] = { ...base, state: "empty" };
      continue;
    }
    const link = byslot[rootOf(d.slot, byslot, s.roots)].link || {};
    const kind = String(d.class).startsWith("03") ? "gpu" : String(d.class).startsWith("0108") ? "nvme" : "other";
    const disk = kind === "nvme" && (inv.disks || []).find((x) => x.bus && x.bus.pci === d.slot);
    const name = (disk && disk.model) || d.name || d.slot;
    const inBrackets = /\[(.*)\]/.exec(name);
    const gts = parseFloat(link.speed);
    out[sid] = {
      ...base, state: "busy", kind, dev: name, short: inBrackets ? inBrackets[1] : name,
      chip: inBrackets ? name.slice(0, name.indexOf("[")).trim() : null,
      width: Math.max(1, Math.min(link.width || s.width, s.width)), gts: Number.isNaN(gts) ? null : gts,
      size: disk ? fmtBytes(disk.size) : null,
    };
  }
  return out;
}

const moduleKind = (m) => [m.type, m.registered ? "R-DIMM" : null].filter(Boolean).join(" ");
const moduleSpeed = (m) => (m.rated_mts ? `${m.rated_mts} МТ/с${m.configured_mts && m.configured_mts !== m.rated_mts ? ` → ${m.configured_mts}` : ""}` : null);

function buildModel(sensors, fans, host, eth, uid) {
  const on = !!(host && host.agent && host.agent.connected && host.inventory);
  const inv = on ? host.inventory : {};
  const list = (sensors && sensors.sensors) || [];
  const find = (src, name) => list.find((s) => s.src === src && s.name === name);
  const hs = (on && host.sensors) || {};

  const sensorsOut = [];
  for (const t of board.temps) {
    const group = t.srcs ? list.filter((s) => t.srcs.test(s.src) && s.kind === "temp") : [find(t.src, t.name)].filter(Boolean);
    const vals = group.filter((s) => s.value !== null && s.value !== undefined);
    if (!vals.length) continue;
    const temp = Math.max(...vals.map((s) => s.value));
    const top = vals.find((s) => s.value === temp);
    sensorsOut.push({ ...t, name: t.title, temp, status: top.status, rows: t.srcs ? vals.map((s) => [s.name, `${s.value.toFixed(1)} °C`]) : null });
  }

  const fanOut = {};
  for (const f of board.fans) {
    if (f.noTach) {
      const z = fans && (fans.zones || []).flatMap((x) => x.fans || []).find((x) => x.dev === f.noTach);
      const duty = z && z.duty !== undefined ? z.duty : null;
      fanOut[f.id] = { conn: duty !== null, rpm: null, pct: duty, chan: f.chan, note: "Тахометра нет: обороты неизвестны, показана заданная мощность." };
    } else {
      const s = find(f.src, f.name);
      const rpm = s && s.value !== undefined ? s.value : null;
      fanOut[f.id] = { conn: rpm !== null, rpm, pct: null, chan: f.chan, duty: s && s.duty !== undefined ? s.duty : null, note: s ? "" : "Нет данных." };
    }
  }

  const pcie = pcieSlots(on, inv);
  for (const [sid, d] of Object.entries(pcie)) {
    if (d.state === "busy" && d.kind === "gpu") {
      d.temp = hs.Host_GPU_Edge;
      d.rpm = hs.Host_GPU_Fan;
      const at = board.gpuCloud(sid);
      if (at && d.temp !== undefined && d.temp !== null) sensorsOut.push({ id: "GPU", name: d.short, title: d.short, at, z: 12, side: "c", noHalo: true, temp: d.temp, status: "ok" });
    }
  }

  // disks by the port they hang from; an agent that doesn't say the port: unknown
  const disks = inv.disks || [];
  const ports = on && (disks.some((d) => d.bus) || disks.length === 0);
  const portState = (ch) => {
    const d = disks.find((x) => x.bus && x.bus.ata && board.ata(x.bus.ata) === ch);
    if (d) return { ch, state: "busy", dev: d.model || d.name, short: (d.model || d.name).split(" ")[0], size: fmtBytes(d.size), name: d.name };
    return { ch, state: ports ? "empty" : "unknown", note: ports ? "" : on ? AGENT_OLD : NO_AGENT };
  };
  const sata = Object.fromEntries(Object.entries(board.sataPorts).map(([sid, ch]) => [sid, portState(ch)]));
  const m2 = Object.fromEntries(Object.entries(board.m2).map(([sid, ch]) => [sid, portState(ch)]));

  // memory: the host's firmware lists the modules, not their slots. All slots full of
  // one kind: which is where doesn't matter. Otherwise it isn't said, so nothing is assigned.
  const modules = inv.memory && inv.memory.modules;
  const ids = Object.entries(board.memory);
  const same = modules && modules.length === ids.length && modules.every((x) => x.size_gb === modules[0].size_gb && x.part === modules[0].part && x.type === modules[0].type);
  const slots = Object.fromEntries(ids.map(([sid, ch]) => [sid, same ? { ch, state: "busy", size: modules[0].size_gb } : { ch, state: "unknown" }]));
  const totalMib = inv.memory && inv.memory.total_mib;
  const sumGb = modules && modules.length ? modules.reduce((a, x) => a + x.size_gb, 0) : null;

  // the network
  const ipv4 = (addrs) => ((addrs || []).find((a) => /^\d+\.\d+\.\d+\.\d+/.test(a)) || "").split("/")[0] || null;
  const link = eth && eth.LinkStatus;
  const ethIp = eth && (eth.IPv4Addresses || []).map((a) => a.Address).filter(Boolean)[0];
  const net = {
    X4: { name: "BMC", known: !!link, link: link === "LinkUp", speed: (eth && eth.SpeedMbps) || null, ip: ethIp || null, mac: eth && eth.MACAddress ? eth.MACAddress.toUpperCase() : null, sub: "сеть BMC + USB 2.0 ×2" },
    X15: { name: "GE1", known: false, link: false, sub: "сеть хоста GE1" },
    X14: { name: "GE0", known: false, link: false, sub: "сеть хоста GE0" },
  };
  for (const n of inv.nics || []) {
    const c = board.nics[n.pci] || board.nics[n.name];
    if (c && net[c]) Object.assign(net[c], { known: true, link: !!n.carrier, speed: n.speed || null, ip: ipv4(n.addrs), mac: n.mac ? n.mac.toUpperCase() : null, iface: n.name });
  }

  // USB: which port of a connector, by its number
  const usb = inv.usb;
  const usbOn = (conn, port) => (usb || []).find((u) => !u.hub && usbPort(u) === port && usbConnector(u) === conn) || null;
  const usbModel = { known: !!usb, X4: board.usb.order.X4.map((p) => usbOn("X4", p)), X19: (usb || []).filter((u) => !u.hub && usbConnector(u) === "X19") };

  const part = id.part ? id.part.replace(/^TVGI/, "ТВГИ") : null;
  return {
    on, inv, bmcLoad: (sensors && sensors.bmc && sensors.bmc.load) || null, sensors: sensorsOut, fans: fanOut, pcie, sata, m2, net, usb: usbModel, uid,
    mem: { total: sumGb || (totalMib ? Math.round(totalMib / 1024) : null), slots, modules: modules || null, same: !!same, kind: modules && modules.length ? moduleKind(modules[0]) : "" },
    board: { serial: id.serial, part, vendor: id.vendor, model: board.name },
  };
}

// what the drawing depends on; values alone change in place
function shapeOf(m) {
  // (the BMC's load: there or not, its value changes in place)
  const pick = (o, f) => Object.entries(o).map(([k, v]) => [k, ...f(v)]);
  return JSON.stringify([
    !!m.bmcLoad, m.sensors.map((s) => s.id), pick(m.fans, (f) => [f.conn]), pick(m.pcie, (d) => [d.state, d.width, d.dev]),
    pick(m.sata, (d) => [d.state, d.dev]), pick(m.m2, (d) => [d.state, d.dev, d.size]),
    pick(m.net, (d) => [d.known, d.link, d.speed]), m.usb.X4.map((u) => !!u), m.uid.on, pick(m.mem.slots, (d) => [d.state, d.size]),
  ]);
}

// ------------------------------------------------------------------ the page
async function refresh() {
  const sensors = await json(`${P}/sensors`);
  const fans = await json(`${P}/fans`);
  const host = await json(`${P}/hostlink`);
  const eth = await api.get("/redfish/v1/Managers/bmc/EthernetInterfaces/eth0").catch(() => null);
  const led = await api.get("/xyz/openbmc_project/led/groups/enclosure_identify").then((r) => r.data).catch(() => null);
  if (!root) return;
  const uid = led && led.Asserted !== undefined ? { known: true, on: !!led.Asserted } : { known: false, on: false };
  model = buildModel(sensors, fans, host, eth, uid);
  const s = shapeOf(model);
  if (s !== shape) {
    const pinned = scene && scene.pinned();
    const zoom = scene && scene.zoomState();
    if (scene) scene.destroy();
    scene = createScene(stage, { size: board.size, view, info, maxHeight: stageHeight });
    if (zoom) scene.setZoomState(zoom);
    board.draw(scene, model);
    scene.finish();
    if (pinned) scene.pin(pinned);
    shape = s;
  } else {
    for (const t of model.sensors) scene.setValue(`S:${t.id}`, t);
    if (model.bmcLoad) scene.setValue("B:load", `LA ${model.bmcLoad[0].toFixed(2)}`);
    for (const [fid, f] of Object.entries(model.fans)) if (f.conn) scene.setValue(`F:${fid}`, fanText(f));
    scene.refreshPopover();
  }
  renderState(sensors, host);
  renderSummary();
  scene.refit();
  showZoom();
}

const ST = { busy: "занято", empty: "свободно", unknown: "неизвестно" };

// the popover
function info(pid) {
  const m = model;
  if (!m) return null;
  if (pid.startsWith("S:")) {
    const s = m.sensors.find((x) => x.id === pid.slice(2));
    if (!s) return null;
    return { title: s.name, sub: "датчик температуры", rows: [...(s.rows || [["Температура", `${Math.round(s.temp)} °C`]]), s.approx ? ["Положение", "приблизительное"] : null], note: s.note };
  }
  if (m.mem.slots[pid]) {
    const d = m.mem.slots[pid];
    const g = m.mem.modules && m.mem.same ? m.mem.modules[0] : null;
    return { title: pid, sub: "разъём памяти", rows: [["Канал", `MC${d.ch}`], ["Состояние", ST[d.state]], d.state === "busy" ? ["Модуль", `${d.size} ГБ ${m.mem.kind}`.trim()] : null,
      g && g.part ? ["Партномер", g.part] : null, g && moduleSpeed(g) ? ["Частота", moduleSpeed(g)] : null, m.mem.total ? ["Всего памяти", `${m.mem.total} ГБ`] : null],
      note: d.state === "unknown" ? memNote(m) : "" };
  }
  if (m.pcie[pid]) {
    const d = m.pcie[pid];
    const r = [["Разъём", `x${d.slot} · PCIe ${board.pcie.gen}`], ["Состояние", ST[d.state]]];
    if (d.state === "busy") {
      r.push(["Подключение", `x${d.width}${d.gts ? ` при ${d.gts} ГТ/с` : ""}`], ["Устройство", d.dev]);
      if (d.chip) r.push(["Чип", d.chip]);
      if (d.size) r.push(["Объём", d.size]);
      if (d.temp !== undefined && d.temp !== null) r.push(["Температура", `${Math.round(d.temp)} °C`]);
      if (d.rpm !== undefined && d.rpm !== null) r.push(["Вентилятор", `${Math.round(d.rpm)} об/мин`]);
    }
    return { title: pid, sub: "слот PCI Express", rows: r, note: d.note };
  }
  if (m.m2[pid] || m.sata[pid]) {
    const d = m.m2[pid] || m.sata[pid];
    return { title: pid, sub: m.m2[pid] ? "M.2 SATA" : "порт SATA", rows: [["Канал", `SATA${d.ch}`], ["Состояние", ST[d.state]], d.dev ? ["Накопитель", d.dev] : null, d.size ? ["Объём", d.size] : null], note: d.note };
  }
  if (m.net[pid]) {
    const d = m.net[pid];
    const rows = [["Линк", !d.known ? "неизвестно" : d.link ? "есть" : "нет"], d.speed ? ["Скорость", `${d.speed} Мбит/с`] : null, d.iface ? ["Интерфейс", d.iface] : null, d.ip ? ["IP", d.ip] : null, d.mac ? ["MAC", d.mac] : null];
    if (pid === "X4") m.usb.X4.forEach((u, i) => rows.push([`USB ${i ? "верхний" : "нижний"}`, !m.usb.known ? "неизвестно" : u ? u.product || u.id : "пусто"]));
    return { title: pid, sub: d.sub, rows, note: pid === "X4" ? (m.usb.known ? "" : m.on ? AGENT_OLD : NO_AGENT) : d.known ? "Какой интерфейс в каком разъёме, выведено по нумерации, кабелем не проверено." : NO_AGENT };
  }
  if (m.fans[pid]) {
    const d = m.fans[pid];
    return { title: pid, sub: "разъём вентилятора", rows: [["Контроллер", d.chan], ["Подключён", d.conn ? "да" : "нет"], d.rpm !== null ? ["Обороты", `${d.rpm} об/мин`] : null, d.pct !== null ? ["Мощность", `${Math.round(d.pct)} %`] : d.duty !== null && d.duty !== undefined ? ["Мощность", `${Math.round(d.duty)} %`] : null], note: d.note };
  }
  if (pid === "UID") return { title: "UID", sub: "светодиод на задней панели", rows: [["Состояние", !m.uid.known ? "нет данных" : m.uid.on ? "горит синим" : "не горит"]] };
  if (pid === "X19") return { title: "X19", sub: "USB 2.0, передняя панель", rows: m.usb.known ? (m.usb.X19.length ? m.usb.X19.map((u) => ["USB", u.product || u.id]) : [["USB", "пусто"]]) : [["USB", "неизвестно"]] };
  if (pid === "SN") return { title: "Серийный номер", sub: "из FRU", rows: [["Значение", m.board.serial || "нет данных"], m.board.vendor ? ["Производитель", m.board.vendor] : null] };
  if (pid === "PN") return { title: "Партномер", sub: "из FRU", rows: [["Значение", m.board.part || "нет данных"]] };
  if (pid === "BOARD") return { title: m.board.model, sub: "244 × 244 мм", rows: [["Процессор", "Эльбрус-8СВ"], m.board.serial ? ["Серийный номер", m.board.serial] : null] };
  const f = board.fixed[pid];
  if (f && pid === "BMC" && m.bmcLoad) return { title: f[0], sub: f[1], rows: [["Load Average", m.bmcLoad.map((x) => x.toFixed(2)).join(" / ")]], note: "За 1, 5 и 15 минут." };
  if (!f) return null;
  const t = pid === "CPU" ? m.sensors.find((s) => s.id === "CPU") : pid === "KPI" ? m.sensors.find((s) => s.id === "KPI") : null;
  return { title: f[0], sub: f[1], rows: t ? [["Температура", `${Math.round(t.temp)} °C`]] : [] };
}

function memNote(m) {
  const n = m.mem.modules ? m.mem.modules.length : null;
  const of = Object.keys(m.mem.slots).length;
  if (!n) return "Хост сообщает только общий объём памяти, без разбивки по разъёмам.";
  return n < of ? `Установлено ${n} из ${of}, какие разъёмы заняты, система не показывает.` : `Установлено ${n} из ${of}, модули разные: какой в каком разъёме, система не показывает.`;
}

function renderState(sensors, host) {
  const out = [];
  const pf = sensors && sensors.platform;
  if (!sensors) out.push(h("div", { class: "alert warn" }, "Датчики платы недоступны."));
  else if (pf === "off") out.push(h("div", { class: "alert" }, "Хост выключен: датчики платы питаются вместе с ним и не читаются."));
  else if (pf && pf !== "on") out.push(h("div", { class: "alert" }, "Хост запускается: датчики появятся через несколько секунд."));
  if (!host || !host.agent || !host.agent.connected) out.push(h("div", { class: "alert" }, "Агент хоста не подключён: занятость разъёмов неизвестна, видны датчики и порт BMC."));
  stateBox.replaceChildren(...out);
}

function renderSummary() {
  const m = model;
  const row = (...c) => h("div", { class: "bs-row" }, ...c);
  const card = (title, big, sub, ...rest) => h("div", { class: "card" }, h("div", { class: "bs-ct" }, title), h("div", { class: "bs-cv" }, big, h("span", {}, sub)), ...rest);
  const slots = Object.entries(m.mem.slots);
  const mg = m.mem.modules && m.mem.modules[0];
  const memCard = card("Память", m.mem.total ? `${m.mem.total} ГБ` : "—", m.mem.kind,
    h("div", { class: "bs-bars" }, slots.map(([sid, d]) => h("div", { class: `bs-bar ${d.state}` }, h("i"), h("b", {}, sid), d.state === "busy" ? `${d.size} ГБ` : d.state === "empty" ? "пусто" : "неизвестно"))),
    m.mem.same && mg.part ? row(h("span", { class: "muted" }, "Партномер"), h("span", {}, mg.part)) : null,
    m.mem.same && moduleSpeed(mg) ? row(h("span", { class: "muted" }, "Частота"), h("span", {}, moduleSpeed(mg))) : null,
    m.mem.modules && m.mem.modules.length && !m.mem.same ? row(h("span", { class: "muted", style: { whiteSpace: "normal" } }, memNote(m))) : null);

  const ps = Object.entries(m.pcie);
  const lanes = ps.reduce((a, [, d]) => a + d.slot, 0);
  const anyKnown = ps.some(([, d]) => d.state !== "unknown");
  const used = ps.reduce((a, [, d]) => a + (d.state === "busy" ? d.width : 0), 0);
  const gts = (ps.find(([, d]) => d.gts) || [null, {}])[1].gts;
  const pcieCard = card(`PCI Express ${board.pcie.gen}`, anyKnown ? String(used) : "—", `из ${lanes} линий в слотах${gts ? ` · ${gts} ГТ/с` : ""}`,
    h("div", { class: "bs-pool" }, h("i", { style: { width: `${Math.round((used / lanes) * 100)}%` } })),
    ps.map(([sid, d]) => row(h("b", {}, sid), h("em", {}, `x${d.state === "busy" ? d.width : d.slot}`), h("span", { class: d.state === "busy" ? "" : "muted" }, d.state === "busy" ? d.dev : ST[d.state] === "свободно" ? "пусто" : ST[d.state]))));

  const disks = [];
  ps.forEach(([sid, d]) => { if (d.state === "busy" && d.kind === "nvme") disks.push([sid, "NVMe", d.dev, d.size]); });
  Object.entries(m.m2).forEach(([sid, d]) => { if (d.state === "busy") disks.push([sid, "SATA", d.dev, d.size]); });
  Object.entries(m.sata).forEach(([sid, d]) => { if (d.state === "busy") disks.push([sid, "SATA", d.dev, d.size]); });
  for (const d of (m.inv.disks || []).filter((x) => x.bus && x.bus.usb)) disks.push([usbConnector({ name: d.bus.usb }) || "USB", "USB", d.model || d.name, fmtBytes(d.size)]);
  const noPort = (m.inv.disks || []).filter((x) => !x.bus);
  for (const d of noPort) disks.push(["?", d.transport || "", d.model || d.name, fmtBytes(d.size)]);
  const word = (n) => (n % 10 === 1 && n % 100 !== 11 ? "накопитель" : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? "накопителя" : "накопителей");
  const diskCard = card("Диски", m.on ? String(disks.length) : "—", m.on ? word(disks.length) : "нет данных",
    m.on ? disks.map((d) => row(h("b", {}, d[0]), h("em", {}, d[1]), h("span", {}, d[2], d[3] ? h("span", { class: "muted" }, ` · ${d[3]}`) : null))) : row(h("span", { class: "muted" }, NO_AGENT)));

  const ns = Object.entries(m.net);
  const up = ns.filter(([, d]) => d.link).length;
  const netCard = card("Сеть", String(up), `из ${ns.length} с линком`,
    ns.flatMap(([sid, d]) => [
      row(h("i", { class: `bs-led ${d.link ? "on" : ""}` }), h("b", {}, sid), h("span", { class: d.link ? "" : "muted" }, `${d.name} · ${!d.known ? "неизвестно" : d.link ? (d.speed ? `${d.speed} Мбит/с` : "линк есть") : "нет линка"}`)),
      d.known ? h("div", { class: "bs-addr" }, d.ip ? h("b", {}, d.ip) : h("i", {}, "нет адреса"), h("span", {}, d.mac || "")) : null,
    ]));
  summary.replaceChildren(memCard, pcieCard, diskCard, netCard);
}
