// Карта платы: одна страница на все платы, данные платы в js/boards/*.js (что
// и где стоит, какой датчик чему соответствует). Плоский чертёж и изометрия: тот
// же DOM под CSS transform. Тепло: ореолы вокруг настоящих датчиков, поля между
// ними нет. Чего данные не говорят, то показано как неизвестное.
import { api, h, fmtBytes } from "../api.js";

const BOARDS = [{ re: /1E8SV-uATX|1E8CB-uATX|TVGI\.469555\.445/i, load: () => import("../boards/1e8sv-uatx.js") }];
const P = "/xyz/openbmc_project/reimu";
const VIEW_KEY = "reimu-map-view";
const json = (p) => api.get(p).then((r) => JSON.parse(r.data.Json)).catch(() => null);
const round = (v) => (v === null || v === undefined ? "—" : Math.round(v));
const S_NS = "http://www.w3.org/2000/svg";

let root, timer, board, stage, wrap, ro, detail, summary, selected;
let nodes = {};
let data = {};
const layers = { heat: true, sensors: true, fans: true, ports: true };

function svg(tag, attrs) {
  const el = document.createElementNS(S_NS, tag);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
  return el;
}

function loadView() {
  try {
    return localStorage.getItem(VIEW_KEY) === "2d" ? "2d" : "iso";
  } catch (e) {
    return "iso";
  }
}

function setView(v) {
  wrap.dataset.view = v;
  root.querySelectorAll("[data-view-btn]").forEach((b) => b.classList.toggle("primary", b.dataset.viewBtn === v));
  try {
    localStorage.setItem(VIEW_KEY, v);
  } catch (e) {
    /* a remembered view is only a convenience */
  }
}

export async function identify() {
  const pns = await json(`${P}/pns`);
  if (!pns) return { name: null };
  const fru = /<product_name>([^<]*)</.exec(pns.fruXml || "");
  const part = /<part_number>([^<]*)</.exec(pns.fruXml || "");
  const model = /^\s*model\s*=\s*"([^"]*)"/m.exec(pns.devtree || "");
  return { name: (model && model[1]) || (fru && fru[1]) || null, part: part && part[1], fru: fru && fru[1] };
}

export async function mount(el) {
  root = el;
  nodes = {};
  selected = null;
  const id = await identify();
  const known = BOARDS.find((b) => [id.name, id.fru, id.part].some((s) => s && b.re.test(s)));
  if (!root) return;
  if (!known) {
    el.append(h("div", { class: "card", id: "map-unknown" }, h("h3", {}, "Карта платы"),
      id.name ? `Для платы «${id.name}» карты пока нет. Датчики есть на странице «Датчики».` : "Плата не определена: нет ни описания платы, ни данных FRU."));
    return;
  }
  board = (await known.load()).default;
  if (!root) return;
  build();
  await refresh();
  timer = setInterval(refresh, 5000);
}

export function unmount() {
  clearInterval(timer);
  if (ro) ro.disconnect();
  root = null;
}

function build() {
  const seg = (v, t) => h("button", { class: "btn small", dataset: { viewBtn: v }, onclick: () => setView(v) }, t);
  const tog = (k, t) => {
    const c = h("input", { type: "checkbox", checked: layers[k], onchange: () => { layers[k] = c.checked; wrap.classList.toggle(`bm-no-${k}`, !c.checked); } });
    return h("label", { class: "small" }, c, " ", t);
  };
  stage = h("div", { class: "bm-stage" });
  wrap = h("div", { class: "bm-wrap", dataset: { view: "iso" } }, stage);
  detail = h("div", { class: "card bm-detail", id: "map-detail" });
  summary = h("div", { id: "map-summary" });
  root.append(
    h("div", { class: "row section" }, h("h2", { class: "m-hide" }, `Карта платы ${board.name}`), h("span", { class: "spacer m-hide" }),
      seg("2d", "Чертёж"), seg("iso", "3D"), tog("heat", "Тепло"), tog("sensors", "Датчики"), tog("fans", "Вентиляторы"), tog("ports", "Разъёмы")),
    h("div", { id: "map-state" }),
    h("div", { class: "bm-main" }, h("div", { class: "card bm-board" }, wrap, legend()), detail),
    summary,
  );
  const [w, ht] = board.size;
  const plate = svg("svg", { viewBox: `0 0 ${w} ${ht}`, class: "bm-pcb" });
  plate.append(svg("rect", { x: 0, y: 0, width: w, height: ht, rx: 1.5, class: "bm-plate" }));
  for (const [x, y] of board.holes) plate.append(svg("circle", { cx: x, cy: y, r: 2, class: "bm-hole" }));
  for (const p of board.parts) plate.append(svg("rect", { x: p.rect[0], y: p.rect[1], width: p.rect[2], height: p.rect[3], class: "bm-foot" }));
  stage.append(plate);
  for (const t of board.temps) stage.append(nodes[`halo:${t.id}`] = h("div", { class: "bm-halo", style: { left: pc(t.at[0], w), top: pc(t.at[1], ht), "--hz": t.hz || 0.4 } }));
  for (const p of board.parts) stage.append(nodes[p.id] = box(p, w, ht));
  for (const t of board.temps) stage.append(nodes[`t:${t.id}`] = marker(t, w, ht));
  setView(loadView());
  showDetail(null);
  const fit = () => {
    const s = Math.min(wrap.clientWidth, 760);
    wrap.style.setProperty("--s", `${s}px`);
    wrap.style.setProperty("--u", `${s / w}px`);
    wrap.classList.toggle("bm-tiny", s < 480);
  };
  ro = new ResizeObserver(fit);
  ro.observe(wrap);
  fit();
}

const pc = (v, of) => `${(v / of) * 100}%`;

function box(p, w, ht) {
  const [x, y, bw, bh] = p.rect;
  const el = h("div", {
    class: `bm-box t-${p.type}${p.approx ? " approx" : ""}${bw < 5 ? " vert" : ""}`, dataset: { id: p.id }, title: p.title,
    style: { left: pc(x, w), top: pc(y, ht), width: pc(bw, w), height: pc(bh, ht), "--z": p.z },
    onclick: () => select(p.id),
  },
    h("i", { class: "bm-f" }), h("i", { class: "bm-l" }),
    h("div", { class: "bm-t" }, p.type === "fan" ? h("b", { class: "bm-spin" }) : null, h("span", { class: "bm-id" }, p.id), h("span", { class: "bm-val" })));
  return el;
}

function marker(t, w, ht) {
  return h("div", { class: `bm-mk${t.approx ? " approx" : ""}`, dataset: { id: t.id }, style: { left: pc(t.at[0], w), top: pc(t.at[1], ht), "--z": t.z }, onclick: () => select(`t:${t.id}`) },
    h("i", { class: "bm-stem" }), h("span", { class: "bm-tag" }));
}

function legend() {
  const it = (c, t) => h("span", { class: "bm-lg" }, h("i", { class: c }), t);
  return h("div", { class: "row small muted bm-legend" }, it("s-used", "занято, есть связь"), it("s-empty", "свободно, нет линка"), it("s-unknown", "неизвестно"), it("s-approx", "≈ место приблизительное"));
}

// ------------------------------------------------------------------ the data
async function refresh() {
  const [sensors, fans, host, eth] = await Promise.all([
    json(`${P}/sensors`), json(`${P}/fans`), json(`${P}/hostlink`),
    api.get("/redfish/v1/Managers/bmc/EthernetInterfaces/eth0").catch(() => null),
  ]);
  if (!root) return;
  data = { sensors, fans, host, eth, facts: {} };
  const facts = data.facts;
  const list = (sensors && sensors.sensors) || [];
  const find = (src, name) => list.find((s) => s.src === src && s.name === name);

  for (const t of board.temps) {
    const group = t.srcs ? list.filter((s) => t.srcs.test(s.src) && s.kind === "temp") : [find(t.src, t.name)].filter(Boolean);
    const vals = group.filter((s) => s.value !== null && s.value !== undefined);
    const v = vals.length ? Math.max(...vals.map((s) => s.value)) : null;
    const top = vals.find((s) => s.value === v) || {};
    showTemp(t, v, top);
    facts[`t:${t.id}`] = { title: t.title, approx: t.approx, note: t.note, rows: vals.length ? group.map((s) => [t.srcs ? s.name : "Температура", `${s.value === null ? "—" : s.value.toFixed(1)} °C`]) : [["Температура", "нет данных"]], };
  }
  for (const p of board.parts.filter((x) => x.type === "fan")) showFan(p, find, fans);
  ports(host, eth);
  renderState(sensors, host);
  renderSummary(host, eth);
  if (selected) showDetail(selected);
}

function heatColor(v, s) {
  if (s.status === "crit") return "#ff3030";
  if (s.status === "warn") return "#ff9020";
  const t = Math.max(0, Math.min(1, (v - 25) / 60));
  return `hsl(${Math.round(210 - 210 * t)} 90% 55%)`;
}

function showTemp(t, v, s) {
  const halo = nodes[`halo:${t.id}`], mk = nodes[`t:${t.id}`];
  mk.classList.toggle("nodata", v === null);
  mk.querySelector(".bm-tag").textContent = v === null ? "—" : `${t.approx ? "≈ " : ""}${Math.round(v)}°`;
  if (v === null) return (halo.style.display = "none");
  const c = heatColor(v, s);
  const k = Math.max(0.35, Math.min(1.2, (v - 15) / 55));
  halo.style.display = "";
  halo.style.setProperty("--c", c);
  halo.style.setProperty("--r", t.r * k);
  mk.style.setProperty("--c", c);
}

function showFan(p, find, fans) {
  const el = nodes[p.id];
  const val = el.querySelector(".bm-val");
  let rpm = null, duty = null, why = "";
  if (p.noTach) {
    const f = fans && (fans.zones || []).flatMap((z) => z.fans || []).find((x) => x.dev === p.noTach);
    duty = f ? f.duty : null;
    why = "Тахометра нет: обороты неизвестны, показана заданная мощность.";
  } else {
    const s = find(p.src, p.name);
    rpm = s ? s.value : null;
    duty = s ? s.duty : null;
    if (duty === undefined) duty = null;
    if (s && rpm === null) why = "Нет сигнала тахометра: вентилятор не подключён или не вращается.";
    if (!s) why = "Нет данных.";
  }
  const spinning = rpm > 0 || (p.noTach && duty > 0);
  el.dataset.state = rpm > 0 ? "used" : rpm === 0 || (p.noTach && duty !== null) ? "idle" : "empty";
  val.textContent = rpm !== null ? `${rpm}` : p.noTach && duty !== null ? `${Math.round(duty)}%` : "";
  const spin = el.querySelector(".bm-spin");
  spin.classList.toggle("on", !!spinning);
  if (spinning) spin.style.animationDuration = `${rpm > 0 ? Math.max(0.25, Math.min(3, 1500 / rpm)) : 1.6}s`;
  const rows = [["Контроллер", p.chan]];
  if (!p.noTach) rows.push(["Обороты", rpm === null ? "—" : `${rpm} об/мин`]);
  if (duty !== null) rows.push(["Мощность", `${Math.round(duty)} %`]);
  data.facts[p.id] = { title: p.title, rows, note: why };
}

function setPort(id, state, rows, note, value) {
  const el = nodes[id];
  if (!el) return;
  el.dataset.state = state;
  const p = board.parts.find((x) => x.id === id);
  el.querySelector(".bm-val").textContent = value || "";
  data.facts[id] = { title: p.title, rows, note, approx: p.approx };
}

const UNK = "Агент хоста не подключён, данных нет.";

function usbOf(u) {
  const m = /^(\d+)-(\d+)/.exec(u.name || "");
  if (!m) return null;
  const c = board.usb.roots[m[1]];
  return c === undefined ? null : board.usb.ports[`${c}${Number(m[2]) - 1}`] || null;
}

// which root port a device hangs from: walk the "parent" of the inventory
function rootOf(slot, byslot, roots) {
  for (let s = slot, n = 0; s && n < 16; n++) {
    if (roots.includes(s)) return s;
    s = byslot[s] && byslot[s].parent;
  }
  return null;
}

function ports(host, eth) {
  const on = host && host.agent && host.agent.connected && host.inventory;
  const inv = on ? host.inventory : {};
  // the BMC's own network is there with no agent at all
  const link = eth && eth.LinkStatus;
  const x4rows = [["BMC LAN", link === "LinkUp" ? `есть линк${eth.SpeedMbps ? `, ${eth.SpeedMbps} Мбит/с` : ""}` : link ? "нет линка" : "неизвестно"]];
  // X4's USB: from the agent
  const usb = inv.usb;
  const usbFor = (id) => (usb || []).filter((u) => usbOf(u) === id);
  const usbRows = (id) => (usb ? (usbFor(id).length ? usbFor(id).map((u) => ["USB", u.product || u.id]) : [["USB", "ничего не определилось"]]) : [["USB", "неизвестно"]]);
  setPort("X4", link === "LinkUp" ? "used" : link ? "empty" : "unknown", [...x4rows, ...usbRows("X4")],
    usb ? "" : on ? "Этот агент не сообщает о USB: его надо обновить." : UNK, eth && eth.SpeedMbps ? `${eth.SpeedMbps}M` : "");
  setPort("X19", usb ? (usbFor("X19").length ? "used" : "empty") : "unknown", usbRows("X19"), usb ? "" : on ? "Этот агент не сообщает о USB: его надо обновить." : UNK);

  // host network
  const seen = {};
  for (const n of inv.nics || []) {
    const id = board.nics[n.pci] || board.nics[n.name];
    if (id) seen[id] = n;
  }
  for (const id of ["X14", "X15"]) {
    const n = seen[id];
    if (!on) setPort(id, "unknown", [["Линк", "неизвестно"]], UNK);
    else if (!n) setPort(id, "unknown", [["Линк", "неизвестно"]], "Агент не показывает этот интерфейс.");
    else setPort(id, n.carrier ? "used" : "empty", [["Интерфейс", n.name], ["Линк", n.carrier ? `есть${n.speed ? `, ${n.speed} Мбит/с` : ""}` : "нет"]], board.nicNote, n.carrier && n.speed ? `${n.speed}M` : "");
  }

  // disks on SATA
  const disks = inv.disks || [];
  const known = disks.some((d) => d.bus) || (on && disks.length === 0);
  for (const p of board.parts.filter((x) => x.sata !== undefined)) {
    const d = disks.find((x) => x.bus && x.bus.ata && board.ata(x.bus.ata) === p.sata);
    if (d) setPort(p.id, "used", [["Диск", d.model || d.name], ["Объём", fmtBytes(d.size)], ["Устройство", d.name]], "");
    else if (known) setPort(p.id, "empty", [["Диск", "не определился"]], "");
    else setPort(p.id, "unknown", [["Диск", "неизвестно"]], on ? "Этот агент не сообщает, на каком порте диск: его надо обновить." : UNK);
  }

  // PCIe slots
  const pci = inv.pci || [];
  const byslot = Object.fromEntries(pci.map((d) => [d.slot, d]));
  const hasTopo = pci.some((d) => d.parent);
  const cards = {};
  for (const d of pci) {
    if (String(d.class).startsWith("0604")) continue;
    const allRoots = board.pcie.groups.flatMap((g) => g.roots);
    const r = rootOf(d.slot, byslot, allRoots);
    if (r) (cards[r] = cards[r] || []).push(d);
  }
  data.cards = cards;
  for (const g of board.pcie.groups) {
    const used = g.roots.filter((r) => cards[r]);
    const names = used.flatMap((r) => cards[r]).filter((d) => !/^0403/.test(d.class)).map((d) => d.name || `${d.vendor}:${d.device}`);
    for (const id of g.slots) {
      const exact = g.roots.find((r) => board.pcie.rootSlot[r] === id);
      if (!on) setPort(id, "unknown", [["Карта", "неизвестно"]], UNK);
      else if (!hasTopo) setPort(id, "unknown", [["Карта", "неизвестно"]], "Этот агент не сообщает топологию PCI: его надо обновить.");
      else if (exact) setPort(id, cards[exact] ? "used" : "empty", cards[exact] ? cards[exact].map((d) => ["Карта", d.name || d.slot]) : [["Карта", "нет"]], "");
      else if (!used.length) setPort(id, "empty", [["Карта", "не определилась"]], g.inferred ? "Порт этого разъёма определён косвенно: на этих портах нет ничего встроенного." : "");
      else if (used.length === g.slots.length) setPort(id, "used", names.map((n) => ["Одна из карт", n]), g.slots.length > 1 ? `Обе карты в разъёмах ${g.slots.join(" и ")}. Какая в каком, система не показывает.` : "");
      else setPort(id, "unknown", names.map((n) => ["Одна из карт", n]), `Занят один из разъёмов ${g.slots.join(", ")}, какой именно, система не показывает.`);
    }
  }

  // memory by slot: the system doesn't say
  for (const p of board.parts.filter((x) => x.type === "dimm")) {
    const edac = (inv.memory && inv.memory.dimms) || [];
    setPort(p.id, "unknown", [["Модуль", "неизвестно"], ["Всего памяти", on && inv.memory ? fmtBytes(inv.memory.total_mib * 1048576) : "неизвестно"]],
      edac.length ? "" : "Хост сообщает только общий объём памяти, без разбивки по разъёмам.");
  }
  for (const p of board.parts.filter((x) => ["chip", "power", "battery"].includes(x.type) || x.id === "X28")) {
    data.facts[p.id] = { title: p.title, rows: [], approx: p.approx };
  }
}

// ------------------------------------------------------------------ text around the map
function renderState(sensors, host) {
  const box = root.querySelector("#map-state");
  const out = [];
  const pf = sensors && sensors.platform;
  if (!sensors) out.push(h("div", { class: "alert warn" }, "Датчики платы недоступны."));
  else if (pf === "off") out.push(h("div", { class: "alert" }, "Хост выключен: датчики платы питаются вместе с ним и не читаются."));
  else if (pf && pf !== "on") out.push(h("div", { class: "alert" }, "Хост запускается: датчики появятся через несколько секунд."));
  if (!host || !host.agent || !host.agent.connected) out.push(h("div", { class: "alert" }, "Агент хоста не подключён: занятость разъёмов неизвестна, видны датчики и порт BMC."));
  box.replaceChildren(...out);
}

function renderSummary(host, eth) {
  const on = host && host.agent && host.agent.connected && host.inventory;
  const inv = on ? host.inventory : null;
  const cards = [];
  if (inv) {
    const s = host.sensors || {};
    const gpu = inv.gpu && inv.gpu.name;
    const pcs = Object.values(data.cards || {}).flat().filter((d) => !/^0403/.test(d.class));
    cards.push(h("div", { class: "card" }, h("h3", {}, "Карты расширения"),
      pcs.length ? h("ul", { class: "bm-ul" }, pcs.map((d) => h("li", {}, d.name || d.slot, d.link ? h("span", { class: "muted small" }, ` x${d.link.width}, ${d.link.speed.replace(" PCIe", "")}`) : null))) : h("div", { class: "muted" }, inv.pci && inv.pci.some((d) => d.parent) ? "Нет" : "Неизвестно: агента надо обновить"),
      gpu && s.Host_GPU_Edge !== undefined && s.Host_GPU_Edge !== null ? h("div", { class: "small muted" }, `GPU: ${round(s.Host_GPU_Edge)} °C, кристалл ${round(s.Host_GPU_Junction)} °C, вентилятор ${round(s.Host_GPU_Fan)} об/мин`) : null));
    cards.push(h("div", { class: "card" }, h("h3", {}, "Диски"),
      (inv.disks || []).length ? h("ul", { class: "bm-ul" }, inv.disks.map((d) => h("li", {}, d.model || d.name, h("span", { class: "muted small" }, ` ${fmtBytes(d.size)}, ${diskWhere(d)}`)))) : h("div", { class: "muted" }, "Нет")));
    cards.push(h("div", { class: "card" }, h("h3", {}, "USB хоста"),
      inv.usb ? (inv.usb.filter((u) => usbOf(u) !== "BMC" && !isBmcChild(u)).length ? h("ul", { class: "bm-ul" }, inv.usb.filter((u) => usbOf(u) !== "BMC" && !isBmcChild(u)).map((u) => h("li", {}, u.product || u.id, h("span", { class: "muted small" }, ` ${usbOf(u) || "разъём неизвестен"}`)))) : h("div", { class: "muted" }, "Ничего не подключено")) : h("div", { class: "muted" }, "Неизвестно: агента надо обновить")));
  }
  summary.className = cards.length ? "grid bm-sum" : "";
  summary.replaceChildren(...cards);
}

const isBmcChild = (u) => /\./.test(u.name || "") && usbOf(u) === "BMC";

function diskWhere(d) {
  if (d.bus && d.bus.ata) {
    const p = board.parts.find((x) => x.sata === board.ata(d.bus.ata));
    return p ? p.id : "SATA";
  }
  if (d.bus && d.bus.usb) return usbOf({ name: d.bus.usb }) || "USB";
  if (d.bus && d.bus.pci) return "PCIe";
  return "разъём неизвестен";
}

function select(id) {
  selected = selected === id ? null : id;
  root.querySelectorAll(".bm-sel").forEach((e) => e.classList.remove("bm-sel"));
  if (selected && nodes[selected]) nodes[selected].classList.add("bm-sel");
  showDetail(selected);
}

function showDetail(id) {
  const f = id && data.facts && data.facts[id];
  if (!f) return detail.replaceChildren(h("h3", {}, "Что где"), h("div", { class: "muted small" }, "Нажмите на разъём или датчик."), h("div", { class: "muted small bm-src" }, `Положения по ${board.doc}. Со знаком ≈ положение приблизительное: в руководстве его нет.`));
  detail.replaceChildren(
    h("h3", {}, f.title),
    f.rows.length ? h("dl", { class: "kv" }, f.rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)])) : null,
    f.approx ? h("div", { class: "small muted" }, "≈ Положение приблизительное: в руководстве его нет.") : null,
    f.note ? h("div", { class: "small muted bm-src" }, f.note) : null,
  );
}
