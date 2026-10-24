// The host as its agent (reimu-hostd) reports it to reimu-hostlink (D-Bus
// xyz.openbmc_project.Reimu.HostLink): agent status and pairing, OS and
// inventory, the video card with its fan modes, ECC, PCIe, disks, events.
import { api, h, toast, fmtBytes } from "../api.js";
import { buildInstaller } from "../lib/agent-pack.js";

const PATH = "/xyz/openbmc_project/reimu/hostlink";
let root, timer, ticker, pairInfo = null;
const ceHistory = []; // [time ms, ce_total] of this page's visit: the ECC trend

const num = (v, d = 0) => (v === undefined || v === null ? "—" : Number(v).toFixed(d));
// a table that scrolls sideways inside its card instead of widening it (its
// header then isn't sticky: it stays on its table)
const scroll = (table) => h("div", { class: "tbl-wrap" }, table);
const kv = (pairs) => h("dl", { class: "kv" }, pairs.filter(([, v]) => v !== undefined && v !== null && v !== "").flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]));
const TRANSPORT = { "ipv6-ll": "IPv6 link-local (внутренняя сеть платы)", ipv4: "IPv4", ipv6: "IPv6" };
const HB = { ok: ["ok", "на связи"], late: ["warn", "нет данных"], lost: ["err", "потерян"], never: ["", "не подключался"] };
const SEV = { critical: "err", error: "err", warning: "warn", notice: "", info: "" };
const MODES = { auto: "Авто (драйвер)", manual: "Ручной", turbo: "Турбо" };
const BOOT = { "native-e2k": "ПНС → Linux E2K (родной)", x86: "ПНС → Lintel → x86" };

function ago(s) {
  if (s === undefined || s === null || s < 0) return "—";
  if (s < 90) return `${s} с назад`;
  if (s < 5400) return `${Math.round(s / 60)} мин назад`;
  return `${(s / 3600).toFixed(1)} ч назад`;
}
const uptime = (s) => (s ? `${Math.floor(s / 86400)} д ${Math.floor((s % 86400) / 3600)} ч ${Math.floor((s % 3600) / 60)} мин` : "—");

async function call(method, args, ok) {
  try {
    const r = await api.post(`${PATH}/action/${method}`, { data: args });
    if (ok) toast(ok, "ok");
    setTimeout(refresh, 700);
    return r && r.data;
  } catch (e) {
    toast(`Хост: ${e.message || e}`, "err");
    return null;
  }
}

export async function mount(el) {
  root = el;
  el.append(h("div", { class: "row section" }, h("h2", { class: "m-hide" }, "Хост"), h("span", { class: "spacer" })), h("div", { id: "host-body" }, h("div", { class: "card" }, "Загрузка…")));
  await refresh();
  timer = setInterval(refresh, 5000);
  ticker = setInterval(tickPair, 1000);
}

export function unmount() {
  clearInterval(timer);
  clearInterval(ticker);
  root = null;
}

async function refresh() {
  let st = null;
  try {
    const r = await api.get(PATH);
    st = JSON.parse(r.data.Json);
  } catch (e) {
    st = null;
  }
  if (!root) return;
  const body = root.querySelector("#host-body");
  // the duty field and the pairing box keep what is being typed/read
  const a = document.activeElement;
  if (a && (a.tagName === "INPUT" || a.tagName === "SELECT") && body.contains(a)) return;
  if (!st) return body.replaceChildren(h("div", { class: "note warn section" }, "Служба связи с хостом (reimu-hostlink) недоступна."));
  const m = st.metrics || {};
  if (m.edac && m.edac.loaded) {
    ceHistory.push([Date.now(), m.edac.ce_total]);
    while (ceHistory.length > 720) ceHistory.shift();
  }
  body.replaceChildren(agentCard(st), h("div", { class: "grid shrink section" }, osCard(st), gpuCard(st), eccCard(st)), pciCard(st), h("div", { class: "grid wide shrink section" }, diskCard(st), nicCard(st)), eventsCard(st));
}

// The host's sensors (reimu-hostlink publishes them on D-Bus for Redfish and
// IPMI too) as rows of the sensors page: there when Redfish has no chassis
// listing them. kind: temp / fan / power as sensors.js groups them.
const SENS = {
  Host_GPU_Edge: ["temp", "edge"],
  Host_GPU_Junction: ["temp", "junction"],
  Host_GPU_Memory: ["temp", "memory"],
  Host_Disk_Max: ["temp"],
  Host_GPU_Fan: ["fan"],
  Host_GPU_Fan_PWM: ["fan", null, true],
  Host_GPU_Power: ["power"],
};
export async function hostSensorRows() {
  let st;
  try {
    st = JSON.parse((await api.get(PATH)).data.Json);
  } catch (e) {
    return [];
  }
  if (!st.paired && !st.agent.connected) return [];
  const th = st.thresholds || {};
  return Object.entries(st.sensors || {})
    .filter(([n]) => SENS[n])
    .map(([n, v]) => {
      const [kind, t, pct] = SENS[n];
      const [warn, crit] = t ? th[t] || [] : [];
      const status = v === null ? { State: "нет сигнала" } : { State: "Enabled", Health: crit !== undefined && v >= crit ? "Critical" : warn !== undefined && v >= warn ? "Warning" : "OK" };
      return { kind, group: "Хост (агент)", name: n, value: v, warn, crit, status, pct, src: "reimu-hostd" };
    });
}

// ------------------------------------------------------------------ agent
function agentCard(st) {
  const ag = st.agent || {};
  const [cls, txt] = HB[st.heartbeat.state] || ["", st.heartbeat.state];
  const pairBtn = h("button", { class: "btn primary small", onclick: () => pair("Pair") }, st.paired ? "Подключить заново" : "Подключить агент хоста");
  const diskBtn = h("button", { class: "btn small", title: "Без сети: BMC покажет хосту USB-диск REIMU-AGENT с установщиком и ключом привязки", onclick: () => pair("AgentDisk") }, "USB-диск с установщиком");
  const unpair = st.paired ? h("button", { class: "btn small danger", onclick: () => confirm("Отозвать ключ агента? Хост перестанет подключаться до новой привязки.") && call("Unpair", [], "Ключ агента отозван") }, "Отключить агент") : null;
  const notes = [];
  if (!st.paired && !pairInfo) notes.push(h("div", { class: "note section" }, "Агент хоста не установлен. «Подключить агент хоста» — команда для терминала хоста: она скачает установщик с этого BMC (по закреплённому ключу HTTPS) и привяжет агент. Без сети — «USB-диск с установщиком»: BMC подключит к хосту диск REIMU-AGENT."));
  const disk = st.disk || {};
  if (pairInfo && pairInfo.disk && !disk.on && pairInfo.diskSeen) pairInfo = null; // the disk went: installed or timed out
  if (pairInfo && pairInfo.disk && disk.on) pairInfo.diskSeen = true;
  if (pairInfo && (st.pairing.active || (pairInfo.disk && disk.on))) notes.push(pairBox(st));
  else pairInfo = null;
  return h(
    "div",
    { class: "card section" },
    h("h3", { style: { flexWrap: "wrap" } }, "Агент хоста", h("span", { class: `badge ${ag.connected ? "ok" : cls}` }, ag.connected ? "подключён" : txt), h("span", { class: "spacer" }), pairBtn, diskBtn, unpair),
    notes,
    kv([
      ["Хост", ag.host],
      ["Версия агента", ag.version],
      ["Связь", ag.transport ? `${TRANSPORT[ag.transport] || ag.transport}, ${ag.peer}` : undefined],
      ["Подключён", ag.connected && ag.since ? new Date(ag.since * 1000).toLocaleString("ru-RU") : undefined],
      ["Последние данные", ag.last_seen ? `${new Date(ag.last_seen * 1000).toLocaleString("ru-RU")} (${ago(st.heartbeat.age)})` : undefined],
      ["Пульс", txt],
      ["Порт BMC", `${st.port} (TLS, ${st.iface || "все интерфейсы"})`],
      ["Сертификат", st.fingerprint ? h("span", { class: "mono small" }, `SHA-256 ${st.fingerprint.slice(0, 32)}…`) : undefined],
    ]),
  );
}

// Pair(): the commands for the host's terminal; AgentDisk(): the same pairing
// on the BMC's USB disk REIMU-AGENT for the host (no network needed)
async function pair(method) {
  const r = await call(method, []);
  if (!r) return;
  try {
    const p = JSON.parse(r);
    p.until = Date.now() + p.expires * 1000;
    pairInfo = p;
    // the box is drawn anew (refresh leaves the card alone while an input has the focus)
    if (document.activeElement && root && root.contains(document.activeElement)) document.activeElement.blur();
  } catch (e) {
    pairInfo = null;
  }
  refresh();
}

const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
function tickPair() {
  if (!root || !pairInfo) return;
  const left = Math.max(0, Math.round((pairInfo.until - Date.now()) / 1000));
  for (const el of root.querySelectorAll(".pair-left")) el.textContent = left ? mmss(left) : "истёк";
}

// the host's commands: the installer fetched from this BMC, its HTTPS key
// pinned (curl --pinnedpubkey: no trust on first use); the installer checks
// the agent's files by their SHA-256, the agent pins reimu-hostlink's own
// certificate (fp in the pairing string); --connect-timeout: an interface without the BMC
// would wait minutes for the TCP handshake
const cmdLL = (p) =>
  `s=; for i in $(ls /sys/class/net); do s=$(curl -fsSg --connect-timeout 3 --pinnedpubkey '${p.https_pin}' -k "https://[${p.link_local}%25$i]/agent/install.sh" 2>/dev/null) && break; done; ` +
  `if [ -n "$s" ]; then printf '%s\\n' "$s" | sudo sh -s -- --pair '${p.pair}'; else echo 'BMC не найден по link-local / BMC not found over link-local' >&2; fi`;
const cmdV4 = (p) => `s=$(curl -fsS --pinnedpubkey '${p.https_pin}' -k https://${p.ipv4}/agent/install.sh) && printf '%s\\n' "$s" | sudo sh -s -- --pair '${p.pair}'`;

function copyText(text, input) {
  const ok = () => toast("Скопировано", "ok");
  if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text).then(ok, () => fallback());
  fallback();
  function fallback() {
    input.focus();
    input.select();
    try {
      document.execCommand("copy");
      ok();
    } catch (e) {
      toast("Выделите и скопируйте вручную (Ctrl+C)", "err");
    }
  }
}

function cmdRow(label, text) {
  const inp = h("input", { class: "mono", readonly: true, value: text, style: { width: "100%" } });
  inp.addEventListener("focus", () => inp.select());
  return h("div", { class: "section" }, h("div", { class: "small" }, label), h("div", { class: "row" }, inp, h("button", { class: "btn small", onclick: () => copyText(text, inp) }, "Копировать")));
}

async function download() {
  try {
    const text = await buildInstaller(pairInfo.pair);
    const a = h("a", { href: URL.createObjectURL(new Blob([text], { type: "text/x-shellscript" })), download: "reimu-agent-install.sh" });
    document.body.append(a);
    a.click();
    setTimeout(() => (URL.revokeObjectURL(a.href), a.remove()), 1000);
  } catch (e) {
    toast(`Установщик: ${e.message || e}`, "err");
  }
}

function pairBox(st) {
  const p = pairInfo;
  const left = Math.max(0, Math.round((p.until - Date.now()) / 1000));
  const head = h("div", {}, h("b", {}, p.disk ? "USB-диск с установщиком: " : "Привязка агента: "), "одноразовый ключ действует ещё ", h("b", { class: "pair-left" }, mmss(left)), ". ",
    h("button", { class: "btn small", onclick: () => pair(p.disk ? "AgentDisk" : "Pair") }, "Новый ключ"),
    p.disk ? h("button", { class: "btn small", onclick: () => call("AgentDiskOff", [], "USB-диск отключён").then(() => (pairInfo = null)) }, "Отключить диск") : null);
  const rows = [];
  if (p.disk) {
    rows.push(h("div", { class: "small section" }, "BMC подключил к хосту USB-диск «REIMU-AGENT» (только чтение): установщик, файлы агента и этот ключ (PAIRING.TXT). На хосте, в терминале:"));
    rows.push(cmdRow("рабочий стол смонтировал диск сам:", "sudo sh /run/media/$USER/REIMU-AGENT/install.sh"));
    rows.push(cmdRow("без графики (смонтировать вручную):", "sudo mount -o ro /dev/disk/by-label/REIMU-AGENT /mnt && sudo sh /mnt/install.sh; sudo umount /mnt"));
    rows.push(h("div", { class: "muted small" }, "Диск отключится сам, когда агент подключится к BMC, или через 15 минут."));
  } else if (!p.https_pin) {
    rows.push(h("div", { class: "note warn" }, "Ключ HTTPS BMC не прочитан (нет /etc/ssl/certs/https/server.pem): безопасной команды нет. Используйте «USB-диск с установщиком» или «Скачать установщик»."));
  } else {
    rows.push(h("div", { class: "small section" }, "На хосте, в терминале (нужны curl и python3; sudo спросит пароль):"));
    if (p.link_local) rows.push(cmdRow(`по внутренней сети платы — IPv6 link-local ${p.link_local}, работает без кабеля и DHCP (интерфейс найдётся сам):`, cmdLL(p)));
    if (p.ipv4) rows.push(cmdRow(`по IPv4 ${p.ipv4}:`, cmdV4(p)));
  }
  return h(
    "div",
    { class: "note section pairbox" },
    head,
    rows,
    h("div", { class: "row section" }, h("button", { class: "btn small", onclick: download }, "Скачать установщик"), h("span", { class: "muted small" }, "файл reimu-agent-install.sh: агент и этот ключ внутри; перенесите на хост и выполните sudo sh reimu-agent-install.sh")),
    h("div", { class: "muted small" }, `Безопасность: команда закрепляет ключ HTTPS этого BMC (${p.https_pin ? p.https_pin.slice(0, 20) + "…" : "—"}) — подменить установщик в сети нельзя; установщик проверяет SHA-256 файлов агента; агент закрепляет сертификат службы связи (SHA-256 ${String(p.fingerprint).slice(0, 16)}…). Ключ привязки одноразовый: никому не передавайте.`),
  );
}

// ------------------------------------------------------------------ OS, CPU, memory
function osCard(st) {
  const inv = st.inventory || {};
  const os = inv.os || {}, cpu = inv.cpu || {}, mem = inv.memory || {}, mt = st.metrics || {};
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "ОС и процессор"),
    inv.os
      ? kv([
          ["ОС", os.name],
          ["Ядро", os.kernel],
          ["Архитектура", os.arch],
          ["Загрузка", BOOT[os.boot] || os.boot],
          ["Имя хоста", os.hostname],
          ["Работает", uptime(mt.uptime || os.uptime)],
          ["Нагрузка", mt.load ? mt.load.map((x) => x.toFixed(2)).join(" / ") : undefined],
          ["Процессор", cpu.model ? `${cpu.model}, ${cpu.threads} ядер${cpu.mhz ? `, ${cpu.mhz} МГц` : ""}` : undefined],
          ["NUMA", cpu.numa && cpu.numa.length ? cpu.numa.map((n) => `узел ${n.node}: ЦП ${n.cpus}, ${fmtBytes(n.mem_mib * 1048576)}`).join("; ") : undefined],
          ["Память", mt.mem ? `${fmtBytes(mt.mem.avail_kib * 1024)} свободно из ${fmtBytes(mt.mem.total_kib * 1024)}` : mem.total_mib ? fmtBytes(mem.total_mib * 1048576) : undefined],
          ["Подкачка", mt.mem && mt.mem.swap_total_kib ? `${fmtBytes(mt.mem.swap_free_kib * 1024)} свободно из ${fmtBytes(mt.mem.swap_total_kib * 1024)}` : undefined],
        ])
      : h("div", { class: "empty" }, "Нет данных от агента"),
    st.agent.connected ? h("div", { class: "row" }, h("button", { class: "btn small", onclick: () => confirm("Корректно выключить ОС хоста?") && call("OsPower", ["poweroff"], "Запрос на выключение ОС отправлен") }, "Выключить ОС"), h("button", { class: "btn small", onclick: () => confirm("Корректно перезагрузить ОС хоста?") && call("OsPower", ["reboot"], "Запрос на перезагрузку ОС отправлен") }, "Перезагрузить ОС")) : null,
  );
}

// ------------------------------------------------------------------ GPU
function gpuCard(st) {
  const g = (st.metrics || {}).gpu;
  const inv = (st.inventory || {}).gpu || {};
  const pol = st.policy || { gpu_fan: { mode: "auto" } };
  const ap = (g && g.policy) || {};
  const th = st.thresholds || {};
  const t = (v, k) => {
    const lim = th[k] || [];
    const cls = v === null || v === undefined ? "" : v >= lim[1] ? "err" : v >= lim[0] ? "warn" : "ok";
    return h("span", { class: `badge ${cls}` }, `${num(v, 0)} °C`);
  };
  const floor = ap.floor || 30;
  const duty = h("input", { type: "number", min: floor, max: 100, step: 1, size: 4, value: pol.gpu_fan.mode === "manual" && pol.gpu_fan.duty ? pol.gpu_fan.duty : 40 });
  const setManual = () => {
    const v = Number(duty.value);
    if (!(v >= 20 && v <= 100)) return toast(`Скважность от ${floor} до 100 %`, "err");
    call("SetGpuFan", ["manual", v], `Вентилятор GPU: ${v} %`);
  };
  duty.addEventListener("keydown", (e) => e.key === "Enter" && setManual());
  const turboMin = h("select", {}, ...[5, 15, 30, 60, 120].map((x) => h("option", { value: x }, `${x} мин`)));
  const mode = st.policy.turbo_left ? "turbo" : pol.gpu_fan.mode;
  const btn = (m, label, fn) => h("button", { class: `btn small${mode === m ? " active" : ""}`, onclick: fn }, label);
  const applied = g && ap.mode ? `${MODES[ap.mode] || ap.mode}${ap.mode === "manual" ? ` ${num(ap.duty)} %` : ""}` : "—";
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Видеокарта", inv.name ? h("span", { class: "muted small" }, inv.name) : null),
    g
      ? [
          kv([
            ["Температура", h("span", {}, "край ", t(g.temp_edge, "edge"), " кристалл ", t(g.temp_junction, "junction"), " память ", t(g.temp_mem, "memory"))],
            ["Вентилятор", `${num(g.fan_rpm)} об/мин, ${num(g.pwm_pct)} % (pwm1_enable ${g.pwm_enable}${g.pwm_enable === 2 ? ": драйвер" : g.pwm_enable === 1 ? ": вручную" : ""})`],
            ["Режим на хосте", `${applied}${ap.reapplied ? ` · восстановлен после сброса GPU: ${ap.reapplied}` : ""}${ap.link_lost ? " · связь с BMC потеряна: драйвер" : ""}`],
            ["Мощность", g.power_w !== null ? `${num(g.power_w, 1)} Вт${g.power_cap_w ? ` из ${num(g.power_cap_w)}` : ""}` : undefined],
            ["Частоты", g.sclk_mhz ? `ядро ${g.sclk_mhz} МГц, память ${g.mclk_mhz} МГц` : undefined],
            ["Загрузка", g.busy_pct !== null ? `${g.busy_pct} % (память ${num(g.mem_busy_pct)} %)` : undefined],
            ["Видеопамять", g.vram_total ? `${fmtBytes(g.vram_used)} из ${fmtBytes(g.vram_total)}` : undefined],
            ["PCIe", g.link && g.link.speed ? `${g.link.speed} x${g.link.width}` : undefined],
            ["Слот", g.slot],
          ]),
        ]
      : h("div", { class: "empty" }, st.agent.connected ? "Видеокарта amdgpu не найдена" : "Нет данных от агента"),
    h("div", { class: "muted small" }, `Политика BMC: ${MODES[mode]}${mode === "manual" ? ` ${pol.gpu_fan.duty} %` : ""}${st.policy.turbo_left ? ` · ещё ${Math.ceil(st.policy.turbo_left / 60)} мин${st.fand_turbo ? " (Турбо вентиляторов BMC)" : ""}` : ""}. Хранится на BMC и восстанавливается после сброса GPU и перезагрузки хоста; минимум на хосте ${floor} %; без связи с BMC более минуты — снова драйвер.`),
    h(
      "div",
      { class: "row" },
      btn("auto", "Авто", () => call("SetGpuFan", ["auto", 0], "Вентилятор GPU: драйвер")),
      h("label", { class: "inline" }, duty, " %"),
      btn("manual", "Ручной", setManual),
      turboMin,
      btn("turbo", "Турбо", () => call("GpuTurbo", [Number(turboMin.value)], `Турбо GPU на ${turboMin.value} мин`)),
      st.policy.turbo_left ? h("button", { class: "btn small", onclick: () => call("GpuTurbo", [0], "Турбо GPU выключено") }, "Стоп") : null,
    ),
  );
}

// ------------------------------------------------------------------ ECC
function eccCard(st) {
  const e = (st.metrics || {}).edac;
  const dimms = ((st.inventory || {}).memory || {}).dimms || [];
  const size = (mc, d) => (dimms.find((x) => x.mc === mc && x.dimm === d) || {}).size_mib;
  let trend = null;
  if (ceHistory.length > 1) {
    const [t0, c0] = ceHistory[0];
    const [t1, c1] = ceHistory[ceHistory.length - 1];
    trend = `за ${Math.max(1, Math.round((t1 - t0) / 60000))} мин на этой странице: ${c1 - c0 > 0 ? `+${c1 - c0}` : "без новых"}`;
  }
  const rows = [];
  if (e && e.loaded)
    for (const mc of e.mcs)
      for (const d of mc.dimms)
        rows.push(h("tr", {}, h("td", {}, d.label || `mc${mc.mc} dimm${d.dimm}`), h("td", { class: "mono" }, size(mc.mc, d.dimm) ? fmtBytes(size(mc.mc, d.dimm) * 1048576) : "—"), h("td", { class: "mono" }, d.ce), h("td", {}, d.ue ? h("span", { class: "badge err" }, d.ue) : "0")));
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Память ECC", e && e.loaded ? h("span", { class: `badge ${e.ue_total ? "err" : e.ce_total ? "warn" : "ok"}` }, e.ue_total ? "некорректируемые ошибки" : e.ce_total ? "исправленные ошибки" : "без ошибок") : null),
    !e
      ? h("div", { class: "empty" }, "Нет данных от агента")
      : !e.loaded
        ? [h("div", { class: "note section" }, "Счётчики ECC недоступны: модуль EDAC (e2k_edac) не загружен."), st.agent.connected ? h("button", { class: "btn small", onclick: () => call("LoadEdac", [], "Запрос modprobe e2k_edac отправлен") }, "Загрузить e2k_edac") : null]
        : [
            kv([
              ["Исправленные (CE)", String(e.ce_total)],
              ["Некорректируемые (UE)", String(e.ue_total)],
              ["Динамика", trend],
            ]),
            rows.length
              ? scroll(h("table", { class: "tbl" }, h("thead", {}, h("tr", {}, h("th", {}, "Модуль"), h("th", {}, "Объём"), h("th", {}, "CE"), h("th", {}, "UE"))), h("tbody", {}, rows)))
              : h(
                  "div",
                  {},
                  scroll(h("table", { class: "tbl" }, h("thead", {}, h("tr", {}, h("th", {}, "Контроллер"), h("th", {}, "CE"), h("th", {}, "UE"))),
                    h("tbody", {}, e.mcs.map((mc) => h("tr", {}, h("td", { class: "mono" }, mc.name || `mc${mc.mc}`), h("td", { class: "mono" }, String(mc.ce + (mc.ce_noinfo || 0))), h("td", {}, mc.ue + (mc.ue_noinfo || 0) ? h("span", { class: "badge err" }, String(mc.ue + (mc.ue_noinfo || 0))) : "0")))))),
                  h("div", { class: "muted small" }, "Драйвер EDAC этой платы считает ошибки на контроллер памяти: разбивки по модулям нет."),
                ),
          ],
  );
}

// ------------------------------------------------------------------ PCIe, disks, NICs
function pciCard(st) {
  const pci = (st.inventory || {}).pci || [];
  const link = (l) => (l ? `${l.speed} x${l.width}${l.max_speed && (l.max_speed !== l.speed || l.max_width !== l.width) ? ` (макс. ${l.max_speed} x${l.max_width})` : ""}` : "—");
  return h(
    "div",
    { class: "card section" },
    h("h3", {}, "Устройства PCIe", h("span", { class: "muted small" }, `${pci.length}`)),
    pci.length
      ? scroll(h(
          "table",
          { class: "tbl" },
          h("thead", {}, h("tr", {}, h("th", {}, "Адрес"), h("th", {}, "Устройство"), h("th", { class: "m-hide" }, "Класс"), h("th", {}, "Драйвер"), h("th", { class: "m-hide" }, "Линк"), h("th", { class: "m-hide" }, "NUMA"))),
          h(
            "tbody",
            {},
            pci.map((d) =>
              h(
                "tr",
                {},
                h("td", { class: "mono small nw" }, d.slot),
                h("td", {}, d.name ? `${d.vendor_name ? d.vendor_name + " " : ""}${d.name}` : h("span", { class: "mono" }, `${d.vendor}:${d.device}`)),
                h("td", { class: "m-hide small" }, d.class_name || d.class),
                h("td", {}, d.driver || h("span", { class: "muted" }, "—")),
                h("td", { class: "m-hide mono small nw" }, link(d.link)),
                h("td", { class: "m-hide" }, d.numa >= 0 ? String(d.numa) : "—"),
              ),
            ),
          ),
        ))
      : h("div", { class: "empty" }, "Нет данных от агента"),
  );
}

function diskCard(st) {
  const disks = (st.inventory || {}).disks || [];
  const health = {};
  for (const d of (st.metrics || {}).disks || []) health[d.name] = d;
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Диски"),
    disks.length
      ? scroll(h(
          "table",
          { class: "tbl" },
          h("tbody", {}, disks.map((d) => {
            const m = health[d.name] || {};
            return h("tr", {}, h("td", { class: "mono nw" }, d.name), h("td", {}, d.model || "—", h("div", { class: "muted small mono" }, d.serial || "")), h("td", { class: "mono nw" }, fmtBytes(d.size)), h("td", { class: "mono nw" }, m.temp !== null && m.temp !== undefined ? `${num(m.temp)} °C` : "—"), h("td", { class: "nw" }, m.smart ? h("span", { class: `badge ${m.smart === "PASSED" ? "ok" : "err"}` }, m.smart === "PASSED" ? "SMART OK" : "SMART: сбой") : h("span", { class: "muted small" }, "SMART —")));
          })),
        ))
      : h("div", { class: "empty" }, "Нет данных от агента"),
  );
}

function nicCard(st) {
  const nics = (st.inventory || {}).nics || [];
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Сеть хоста"),
    nics.length
      ? scroll(h("table", { class: "tbl" }, h("tbody", {}, nics.map((n) => h("tr", {}, h("td", { class: "mono nw" }, n.name), h("td", { class: "nw" }, h("span", { class: `badge ${n.state === "up" ? "ok" : ""}` }, n.state), n.speed ? ` ${n.speed} Мбит/с` : ""), h("td", { class: "mono small" }, h("div", { class: "nw" }, n.mac), (n.addrs || []).map((a) => h("div", { class: "muted nw" }, a))))))))
      : h("div", { class: "empty" }, "Нет данных от агента"),
  );
}

function eventsCard(st) {
  const ev = (st.events || []).slice().reverse();
  return h(
    "div",
    { class: "card section" },
    h("h3", {}, "События хоста", h("span", { class: "muted small" }, "также в журнале BMC")),
    ev.length
      ? scroll(h("table", { class: "tbl" }, h("tbody", {}, ev.map((e) => h("tr", {}, h("td", { class: "mono small nw" }, new Date(e.time * 1000).toLocaleString("ru-RU")), h("td", {}, h("span", { class: `badge ${SEV[e.sev] || ""}` }, e.sev)), h("td", { style: { overflowWrap: "anywhere" } }, e.text))))))
      : h("div", { class: "empty" }, "Событий нет"),
  );
}
