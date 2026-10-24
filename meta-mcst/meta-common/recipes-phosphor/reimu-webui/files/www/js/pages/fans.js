import { api, h, toast } from "../api.js";

// Fan control (reimu-fand, D-Bus xyz.openbmc_project.Reimu.Fans): its state
// and the zone controls, a card of the sensors page.
const PATH = "/xyz/openbmc_project/reimu/fans";

export async function fanState() {
  try {
    const r = await api.get(PATH);
    return JSON.parse(r.data.Json);
  } catch (e) {
    return null;
  }
}

const MODES = { auto: "Авто", manual: "Ручной", full: "100 %" };
const INPUTS = { pcs: "процессор", vrm: "VRM", iohub: "IOHUB", gpu: "GPU хоста" };
const num = (v, d = 0) => (v === undefined || v === null ? "—" : Number(v).toFixed(d));
const p2 = (n) => String(n).padStart(2, "0");
const mmss = (s) => (s >= 3600 ? `${Math.floor(s / 3600)}:${p2(Math.floor(s / 60) % 60)}:${p2(s % 60)}` : `${Math.floor(s / 60)}:${p2(s % 60)}`);
const UNTIL_OFF = 4294967295;

async function call(method, args, refresh) {
  try {
    await api.post(`${PATH}/action/${method}`, { data: args });
  } catch (e) {
    toast(`Вентиляторы: ${e.message || e}`, "err");
  }
  if (refresh) refresh();
  updateChip();
}

// Турбо: every fan at full scale for a while, to dump heat fast
function turboRow(st, refresh) {
  if (st.turbo)
    return h(
      "div",
      { class: "row section", id: "fan-turbo" },
      h("span", { class: "badge warn" }, `Турбо: осталось ${mmss(st.turbo_left)}`),
      h("span", { class: "muted small" }, "все вентиляторы на полную"),
      h("span", { class: "spacer" }),
      h("button", { class: "btn small", onclick: () => call("SetTurbo", [0], refresh) }, "Выключить турбо"),
    );
  return h(
    "div",
    { class: "row section", id: "fan-turbo" },
    h("b", {}, "Турбо"),
    h("span", { class: "muted small" }, "все вентиляторы на полную, чтобы быстро сбросить тепло:"),
    [15, 30, 60].map((m) => h("button", { class: "btn small", onclick: () => call("SetTurbo", [m], refresh) }, m < 60 ? `${m} мин` : "1 ч")),
    h("button", { class: "btn small", onclick: () => call("SetTurbo", [UNTIL_OFF], refresh) }, `до выключения (≤ ${Math.round((st.turbo_max_min || 720) / 60)} ч)`),
  );
}

// The quick toggle in the top bar, on every page: off -> 30 min, on -> off
let chip = null,
  chipTimer = null;

export async function updateChip(st) {
  if (!chip) return;
  if (st === undefined) st = await fanState();
  if (!st) {
    chip.classList.add("hidden");
    return;
  }
  chip.classList.remove("hidden");
  chip.classList.toggle("active", !!st.turbo);
  chip.dataset.on = st.turbo ? "1" : "";
  chip.textContent = st.turbo ? `Турбо ${Math.ceil(st.turbo_left / 60)} мин` : "Турбо";
  chip.title = st.turbo ? `Турбо: все вентиляторы на полную ещё ${mmss(st.turbo_left)}; нажать — выключить` : "Турбо: все вентиляторы на полную на 30 мин";
}

export function startTurboChip() {
  if (!chip) {
    const bar = document.querySelector(".topbar-right");
    if (!bar) return;
    chip = h("button", { class: "btn small turbo-chip hidden", id: "turbo-chip" }, "Турбо");
    chip.addEventListener("click", () => call("SetTurbo", [chip.dataset.on ? 0 : 30], () => document.dispatchEvent(new CustomEvent("reimu-fans"))));
    bar.prepend(chip);
  }
  updateChip();
  clearInterval(chipTimer);
  chipTimer = setInterval(() => updateChip(), 10000);
}

export function stopTurboChip() {
  clearInterval(chipTimer);
}

function zoneBlock(st, z, refresh) {
  const run = st.host === "on" || st.host === "unknown";
  const fan = z.fans.find((f) => f.rpm !== undefined);
  const inputs = z.inputs
    // the host's GPU only while reimu-hostlink reports it
    .filter((i) => i.used && !(i.id === "gpu" && i.temp === null))
    .map((i) => `${INPUTS[i.id] || i.id} ${num(i.temp, 1)} °C → ${num(i.demand)} %`)
    .join(" · ");
  const duty = h("input", { type: "number", min: st.floor, max: 100, step: 1, value: Math.round(z.manual_duty !== null ? z.manual_duty : z.duty !== null ? z.duty : 100), size: 4 });
  const setDuty = () => {
    const v = Number(duty.value);
    if (!(v >= st.floor && v <= 100)) return toast(`Скважность от ${st.floor} до 100 %`, "err");
    call("SetDuty", [z.id, v], refresh);
  };
  duty.addEventListener("keydown", (e) => e.key === "Enter" && setDuty());
  const modeBtn = (m) =>
    h("button", { class: `btn small${z.mode === m ? " active" : ""}`, onclick: () => (m === "manual" ? setDuty() : call("SetMode", [z.id, m], refresh)) }, MODES[m]);
  return h(
    "div",
    { class: "section" },
    h("div", { class: "row" }, h("b", {}, z.name), h("span", { class: "spacer" }), z.failsafe ? h("span", { class: "badge err" }, "100 %: нет данных") : h("span", { class: "badge" }, MODES[z.mode] || z.mode)),
    h(
      "div",
      { class: "mono" },
      run ? `${num(z.duty)} %` : "—",
      run && z.mode === "auto" && z.demand !== null && Math.round(z.demand) !== Math.round(z.duty) ? h("span", { class: "muted" }, ` (по кривым ${num(z.demand)} %)`) : null,
      fan && run ? ` · вентилятор ${num(fan.duty)} %, ${num(fan.rpm)} об/мин` : null,
    ),
    run && inputs ? h("div", { class: "muted small" }, inputs) : null,
    run && z.reason ? h("div", { class: "small" }, z.reason) : null,
    h(
      "div",
      { class: "row" },
      modeBtn("auto"),
      modeBtn("full"),
      h("label", { class: "inline" }, duty, " %"),
      modeBtn("manual"),
      z.mode === "manual"
        ? [h("span", { class: "muted small" }, `авто через ${mmss(z.manual_left || 0)}`), h("button", { class: "btn small", onclick: () => call("Confirm", [z.id], refresh) }, "Продлить")]
        : null,
    ),
  );
}

function card(st, refresh) {
  if (!st) return h("div", { class: "note warn section" }, "Сервис управления вентиляторами (reimu-fand) недоступен.");
  const notes = [];
  if (st.host === "off") notes.push(h("div", { class: "note section" }, "Платформа выключена: вентиляторы обесточены, управление начнётся после её включения."));
  if (st.host === "starting") notes.push(h("div", { class: "note section" }, `Платформа включается: управление начнётся через ${st.settle_left || 0} с (шина I²C не трогается во время её запуска).`));
  if (st.host === "pending") notes.push(h("div", { class: "note section" }, "Состояние питания платформы ещё не известно."));
  if (st.full_left) notes.push(h("div", { class: "note section" }, `Запуск: 100 % ещё ${st.full_left} с, затем по кривым.`));
  if (st.alarm) notes.push(h("div", { class: "note warn section" }, `Авария: ${st.alarm_text}`));
  if (st.config_errors && st.config_errors.length) notes.push(h("div", { class: "note warn section" }, `Ошибки в /etc/reimu-fand.conf (взяты значения по умолчанию): ${st.config_errors.join("; ")}`));
  return h(
    "div",
    { class: "card section" },
    h(
      "h3",
      {},
      "Управление вентиляторами",
      h("span", { class: "muted small" }, `минимум ${st.floor} % · ручной режим — до ${Math.round(st.manual_timeout / 60)} мин`),
      st.alarm ? h("button", { class: "btn small", onclick: () => call("ClearAlarms", [], refresh) }, "Сбросить аварию") : null,
    ),
    turboRow(st, refresh),
    notes,
    st.zones.map((z) => zoneBlock(st, z, refresh)),
  );
}

// Re-rendered on every refresh, except while its duty field is being edited
export function renderFans(el, st, refresh) {
  if (!el) return;
  updateChip(st || null);
  const a = document.activeElement;
  if (a && a.tagName === "INPUT" && el.contains(a)) return;
  el.replaceChildren(card(st, refresh));
}
