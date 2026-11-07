import { api, h, toast } from "../api.js";

// The overheat protection (overheatd; D-Bus xyz.openbmc_project.Reimu.Overheat
// of reimu-hwmon): the toggle of the power actions, a card of the settings page,
// and what the sensors page must show about it.
const PATH = "/xyz/openbmc_project/reimu/overheat";
export const OFF_WARNING = "Если отключить, BMC не будет выключать хост при перегреве. Встроенная защита процессора может не сработать.";

export async function overheatState() {
  try {
    const r = await api.get(PATH);
    return JSON.parse(r.data.Json);
  } catch (e) {
    return null;
  }
}

async function call(method, args, okText) {
  try {
    await api.post(`${PATH}/action/${method}`, { data: args });
    if (okText) toast(okText, "ok");
    return true;
  } catch (e) {
    toast(`Защита от перегрева: ${e.message || e}`, "err");
    return false;
  }
}

const reasonText = (st) => (st.emergency || "").trim().split("\n").join("; ");

// Settings: the switch, with its warning; off only after a confirmation
export function overheatCard(st, refresh) {
  const on = h("input", { type: "checkbox", id: "overheat-poweroff", checked: !st || st.poweroff !== false, disabled: !st });
  on.addEventListener("change", async () => {
    if (!on.checked && !confirm(`Отключить выключение хоста при перегреве?\n\n${OFF_WARNING}`)) {
      on.checked = true;
      return;
    }
    await call("SetPowerOff", [on.checked], on.checked ? "Выключение при перегреве включено" : "Выключение при перегреве отключено");
    refresh();
  });
  const limits = st && st.limit !== null && st.limit !== undefined ? `Сигнал при ${st.limit} °C, корректное выключение ОС при ${st.shutdown} °C, принудительное при ${st.off} °C.` : "";
  return h(
    "div",
    { class: "card", id: "overheat-card" },
    h("h3", {}, "Защита от перегрева"),
    !st ? h("div", { class: "note warn" }, "Сервис reimu-hwmon недоступен.") : null,
    st && st.poweroff === false ? h("div", { class: "row section" }, h("span", { class: "badge warn", id: "overheat-card-off" }, "Выключение при перегреве отключено")) : null,
    h("label", { class: "inline" }, on, "Выключать хост при перегреве"),
    h("p", { class: "muted small" }, limits),
    h("p", { class: "small" }, OFF_WARNING),
    h("p", { class: "muted small" }, "Сигнал, светодиод, журнал и вентиляторы работают и при отключённом выключении."),
  );
}

// Sensors: the protection is off, or an emergency power-off happened
export function overheatNotes(st, refresh) {
  if (!st) return [];
  const notes = [];
  if (st.poweroff === false)
    notes.push(h("div", { class: "note warn section", id: "overheat-off" }, h("span", { class: "badge warn" }, "Защита от перегрева отключена"), " BMC не выключит хост при перегреве: только сигнал. Включается в настройках BMC."));
  if (st.status === "blind")
    notes.push(h("div", { class: "note warn section", id: "overheat-blind" }, h("span", { class: "badge warn" }, st.blind_alarm ? `Датчики процессора не читаются уже ${Math.floor(st.blind_sec / 60)} мин` : "Датчики процессора не читаются"), " Хост включён, температура процессора неизвестна: BMC не выключит его по температуре. Вентиляторы — на полную."));
  if (st.emergency) {
    const clear = h("button", { class: "btn small", id: "overheat-clear" }, "Снять отметку");
    clear.addEventListener("click", async () => {
      await call("ClearEmergency", [], "Отметка снята");
      refresh();
    });
    notes.push(h("div", { class: "note warn section", id: "overheat-emergency" }, h("b", {}, "Аварийное выключение из-за перегрева. "), reasonText(st), ". Автоматическое включение хоста запрещено до включения вручную. ", clear));
  }
  return notes;
}
