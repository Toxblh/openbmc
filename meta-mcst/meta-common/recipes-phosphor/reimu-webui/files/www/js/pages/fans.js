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
const INPUTS = { pcs: "процессор", vrm: "VRM", iohub: "IOHUB" };
const num = (v, d = 0) => (v === undefined || v === null ? "—" : Number(v).toFixed(d));
const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;

async function call(method, args, refresh) {
  try {
    await api.post(`${PATH}/action/${method}`, { data: args });
  } catch (e) {
    toast(`Вентиляторы: ${e.message || e}`, "err");
  }
  if (refresh) refresh();
}

function zoneBlock(st, z, refresh) {
  const run = st.host === "on" || st.host === "unknown";
  const fan = z.fans.find((f) => f.rpm !== undefined);
  const inputs = z.inputs
    .filter((i) => i.used)
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
    notes,
    st.zones.map((z) => zoneBlock(st, z, refresh)),
  );
}

// Re-rendered on every refresh, except while its duty field is being edited
export function renderFans(el, st, refresh) {
  if (!el) return;
  const a = document.activeElement;
  if (a && a.tagName === "INPUT" && el.contains(a)) return;
  el.replaceChildren(card(st, refresh));
}
