// BMC firmware card of the settings page: image upload through Redfish
// UpdateService and, on boards with two BMC flash chips, the reliable
// upgrade state from reimu-firmwared (/xyz/openbmc_project/reimu/firmware):
// which chip is running, whether a new image waits for its health check,
// the backup chip contents and the sync/verify/restore jobs.

import { api, h, toast, fmtBytes } from "../api.js";

const FW = "/xyz/openbmc_project/reimu/firmware";

const kv = (pairs) => h("dl", { class: "kv" }, pairs.filter(([, v]) => v !== undefined && v !== null && v !== "").flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]));
const when = (s) => (s ? new Date(s * 1000).toLocaleString() : "—");
const JOBS = { verify: "сверка чипов", "sync-backup": "копирование на резервный чип", "restore-primary": "восстановление основного чипа", "prepare-update": "подготовка к обновлению" };

export async function getStatus() {
  try {
    const r = await api.get(FW);
    return JSON.parse(r.data.Json);
  } catch (e) {
    return e.status === 404 ? null : undefined; // null: no reliable upgrade on this board; undefined: request failed
  }
}

async function run(action) {
  const r = await api.post(`${FW}/action/Run`, { data: [action] });
  return JSON.parse(r.data);
}

// Creates the card; `onReboot` reboots the BMC (shared with the BMC card).
export function firmwareCard(mgr, onReboot) {
  const card = h("div", { class: "card span2", id: "fw-card" }, h("h3", {}, "Прошивка BMC"), h("div", { class: "muted" }, "Загрузка…"));
  let timer = null;
  let alive = true;
  let last = null;

  async function refresh() {
    clearTimeout(timer);
    if (!alive) return;
    let st = await getStatus();
    if (!alive) return;
    // a failed request (BMC busy) keeps the last state and retries
    if (st === undefined && last) {
      timer = setTimeout(refresh, 3000);
      return;
    }
    if (st === undefined) st = null;
    const wasRunning = last && last.job && last.job.state === "running";
    last = st;
    card.replaceChildren(...render(st));
    // follow a running job, and look once more after it ended (results)
    if (st && st.job && st.job.state === "running") timer = setTimeout(refresh, 2000);
    else if (wasRunning) timer = setTimeout(refresh, 1500);
  }
  function stop() {
    alive = false;
    clearTimeout(timer);
  }

  async function action(name, question) {
    if (question && !confirm(question)) return;
    try {
      const { ok, message } = await run(name);
      toast(message, ok ? "ok" : "err");
    } catch (e) {
      toast(`Ошибка: ${e.message}`, "err");
    }
    setTimeout(refresh, 500);
  }

  function render(st) {
    const out = [h("h3", {}, "Прошивка BMC")];
    const backup = st && st.bootedFrom === "backup";
    if (!st) {
      out.push(kv([["Версия", (mgr && mgr.FirmwareVersion) || "—"]]), h("p", { class: "muted small" }, "Резервирования прошивки нет (один флеш-чип или сервис reimu-firmwared недоступен): неудачное обновление придётся восстанавливать программатором."));
    } else if (!st.strap || !st.dual) {
      out.push(kv([["Версия", st.running.name || st.running.version]]), h("p", { class: "muted small" }, !st.strap ? "Плата не настроена на загрузку со второго чипа (strap SCU70[17]): откат при неудачном обновлении не работает." : "Второй флеш-чип BMC не найден: откат при неудачном обновлении не работает."));
    } else {
      const badge = backup ? h("span", { class: "badge err" }, "резервный чип") : h("span", { class: "badge ok" }, "основной чип");
      const sync = st.inSync === true ? h("span", { class: "badge ok" }, "совпадает с рабочим") : st.inSync === false ? h("span", { class: "badge warn" }, "отличается") : h("span", { class: "badge" }, "не проверялось");
      out.push(
        kv([
          ["Работает с", badge],
          ["Версия", st.running.name || st.running.version],
          ["Другой чип", h("span", {}, (st.other && st.other.version) || "—", " ", sync)],
          ["Последнее копирование", st.lastMirror ? `${when(st.lastMirror.date)} (${st.lastMirror.version})` : "—"],
          ["Подтверждена", st.committed ? `${st.committed.version}, ${when(st.committed.date)}` : "—"],
          ["WDT2", st.wdt2.enabled ? h("span", { class: "badge warn" }, `взведён, ${st.wdt2.remaining} с`) : "выключен"],
        ]),
      );
      if (backup) {
        out.push(
          h("div", { class: "alert err" }, "BMC загрузился с резервного чипа: образ на основном чипе не запустился", st.fallback ? ` (${when(st.fallback)})` : "", ". Восстановите основной чип копией рабочего образа и загрузитесь с него. Обновление прошивки до этого запрещено."),
        );
      }
      if (st.pending) {
        out.push(h("div", { class: "alert warn" }, `Новый образ ожидает подтверждения: ${st.health && st.health !== "ok" ? `проверка не пройдена (${st.health})` : "проверка работоспособности идёт"}. Если он не станет работоспособным, WDT2 перезапустит BMC с резервного чипа.`));
      }
      const job = st.job;
      if (job) {
        const running = job.state === "running";
        out.push(
          h(
            "div",
            { class: "section" },
            h("div", { class: "small" }, `${JOBS[job.action] || job.action}: `, h("span", { class: `badge ${job.state === "ok" ? "ok" : job.state === "failed" ? "err" : "warn"}` }, running ? "выполняется" : job.state === "ok" ? "готово" : "ошибка"), " ", h("span", { class: "muted" }, job.message)),
            running ? h("div", { class: "progress" }, h("div", { style: { width: `${job.progress || 0}%` } })) : null,
          ),
        );
      }
      const busy = job && job.state === "running";
      const btn = (text, name, q, cls = "btn") => {
        const b = h("button", { class: cls, disabled: busy }, text);
        b.addEventListener("click", () => action(name, q));
        return b;
      };
      const row = h("div", { class: "row" });
      if (backup) {
        row.append(
          btn("Восстановить основной чип", "restore-primary", "Записать рабочий образ с резервного чипа на основной? Раздел настроек (rwfs) основного чипа не изменяется.", "btn primary"),
          btn("Загрузиться с основного", "reboot-primary", "Перезагрузить BMC с основного чипа? Платформа будет перезапущена вместе с BMC.", "btn danger"),
        );
      } else {
        row.append(btn("Сверить чипы", "verify"), btn("Обновить резервную копию", "sync-backup", "Скопировать работающий образ на резервный чип?"));
        if (st.pending) row.append(btn("Подтвердить образ", "commit", "Считать текущий образ рабочим и выключить WDT2?"), btn("Отменить ожидание", "cancel-update"));
      }
      out.push(row);
    }
    out.push(uploadSection(st, backup, onReboot));
    return out;
  }

  refresh();
  card.refresh = refresh;
  card.stop = stop;
  return card;
}

function uploadSection(st, backup, onReboot) {
  const file = h("input", { type: "file", accept: ".tar,.mtd.tar,application/x-tar" });
  const go = h("button", { class: "btn primary", disabled: true }, "Загрузить и активировать");
  const bar = h("div", { class: "progress hidden" }, h("div", {}));
  const note = h("div", { class: "muted small" });
  file.addEventListener("change", () => (go.disabled = !file.files.length || backup));
  go.addEventListener("click", async () => {
    const f = file.files[0];
    if (!f) return;
    const head = new Uint8Array(await f.slice(257, 262).arrayBuffer());
    if (String.fromCharCode(...head) !== "ustar") return toast("Это не tar-архив образа (нужен obmc-phosphor-image-*.static.mtd.tar)", "err");
    const guard = st && st.strap && st.dual ? "Текущий образ будет сохранён на резервный чип, новый должен пройти проверку после перезагрузки, иначе BMC вернётся к резервному." : "Резервного чипа нет: при неудачном обновлении BMC придётся восстанавливать программатором.";
    if (!confirm(`Обновить прошивку BMC образом ${f.name} (${fmtBytes(f.size)})?\n\n${guard}`)) return;
    go.disabled = file.disabled = true;
    bar.classList.remove("hidden");
    try {
      try {
        await upload(f, (p) => (bar.firstChild.style.width = `${Math.round(p * 100)}%`));
      } catch (e) {
        // bmcweb gives up waiting for the updater after a while, though the
        // image may still be accepted: look for it staged for flashing
        if (!(st && (await stagedSoon()))) throw e;
      }
      note.replaceChildren("Образ принят и активирован. Он будет записан во флеш при перезагрузке BMC (около 7 минут, платформа перезапустится).", " ", reboot(onReboot));
      toast("Образ активирован", "ok");
    } catch (e) {
      toast(`Обновление не удалось: ${e.message}`, "err");
      go.disabled = file.disabled = false;
    } finally {
      bar.classList.add("hidden");
    }
  });
  return h(
    "div",
    { class: "section" },
    h("h3", {}, "Обновление"),
    backup ? h("p", { class: "muted small" }, "Недоступно при работе с резервного чипа.") : null,
    h("div", { class: "row" }, file, go),
    bar,
    note,
    h("p", { class: "muted small" }, "Также из консоли: firmware-updater <URL образа>; прошивка ПНС: update-host-flash."),
  );
}

async function stagedSoon() {
  for (let i = 0; i < 20; i++) {
    const s = await getStatus();
    if (s && s.staged && s.staged.length) return true;
    await new Promise((r) => setTimeout(r, 3000));
  }
  return false;
}

function reboot(onReboot) {
  const b = h("button", { class: "btn danger small" }, "Перезагрузить сейчас");
  b.addEventListener("click", onReboot);
  return b;
}

// XHR (unlike fetch) reports upload progress; bmcweb stores the body in
// /tmp/images and answers once phosphor-image-updater accepted the image.
function upload(file, onProgress) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open("POST", "/redfish/v1/UpdateService");
    x.setRequestHeader("Content-Type", "application/octet-stream");
    x.setRequestHeader("X-XSRF-TOKEN", api.csrfToken());
    x.upload.onprogress = (e) => e.lengthComputable && onProgress(e.loaded / e.total);
    x.onload = () => {
      if (x.status >= 200 && x.status < 300) return resolve();
      let msg = `${x.status} ${x.statusText}`;
      try {
        const b = JSON.parse(x.responseText);
        const ext = b.error && b.error["@Message.ExtendedInfo"];
        msg = (ext && ext.map((m) => m.Message).join("; ")) || (b.error && b.error.message) || msg;
      } catch (e) {
        /* not JSON */
      }
      reject(new Error(x.status === 413 ? "образ больше допустимого размера запроса bmcweb" : msg));
    };
    x.onerror = () => reject(new Error("соединение прервано"));
    x.send(file);
  });
}
