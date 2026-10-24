// PNS flash card of the hardware page: the versions on the chip, backups
// (read, download, write back), a new image (upload, check, write) and the
// job progress, from reimu-pnsd (/xyz/openbmc_project/reimu/pns_flash), which
// runs reimu-pns-flash: the flash is switched to the BMC only while the
// platform is off, every write is read back and compared.

import { api, h, toast, fmtBytes } from "../api.js";
import { appState } from "../app.js";

const PF = "/xyz/openbmc_project/reimu/pns_flash";
const CHUNK = 512 * 1024;
const MAGIC = [0x12, 0x50, 0x00, 0x00, 0x20, 0x00, 0x00, 0xc0];

const kv = (pairs) => h("dl", { class: "kv" }, pairs.filter(([, v]) => v !== undefined && v !== null && v !== "").flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]));
const when = (s) => (s ? new Date(s * 1000).toLocaleString() : "—");
const hex = (n) => "0x" + Number(n).toString(16);
const SOURCES = { boot: "при загрузке BMC", read: "чтение из веб-интерфейса или консоли", write: "после записи", backup: "перед записью" };
const JOBS = { read: "Чтение флеша", write: "Запись флеша", version: "Чтение версии" };
const STATES = { running: ["выполняется", "warn"], ok: ["готово", "ok"], failed: ["ошибка", "err"], interrupted: ["прервано", "err"] };

async function status() {
  const r = await api.get(PF);
  return JSON.parse(r.data.Json);
}

async function call(method, args) {
  const r = await api.post(`${PF}/action/${method}`, { data: args });
  return JSON.parse(r.data);
}

const run = (action, arg = "") => call("Run", [action, arg]);

const hostOn = () => appState.power !== "Off";

function sha256hex(buf) {
  return crypto.subtle.digest("SHA-256", buf).then((d) => [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join(""));
}

function b64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

function unb64(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

const allFF = (b) => b.every((x) => x === 0xff);

// the image into reimu-pnsd: chunks of base64, erased (0xff) ones as "ff:N"
async function uploadImage(file, onProgress) {
  const data = new Uint8Array(await file.arrayBuffer());
  for (let off = 0; off < data.length || off === 0; off += CHUNK) {
    const part = data.subarray(off, Math.min(off + CHUNK, data.length));
    const r = await call("Upload", [off, allFF(part) ? `ff:${part.length}` : b64(part)]);
    if (!r.ok) throw new Error(r.message);
    onProgress(Math.min(1, (off + part.length) / data.length));
    if (!data.length) break;
  }
  return run("upload-done", await sha256hex(data));
}

// a backup is kept gzipped on the BMC: the first chunk unpacks it there and
// tells its size
async function downloadBackup(b, onProgress) {
  let out = null;
  for (let off = 0; !out || off < out.length; off += CHUNK) {
    const r = await call("Download", [b.name, off, CHUNK]);
    if (!r.ok) throw new Error(r.message);
    if (!out) out = new Uint8Array(r.size);
    if (r.ff) out.fill(0xff, off, off + r.length);
    else out.set(unb64(r.data), off);
    onProgress((off + r.length) / out.length);
    if (!r.length) break;
  }
  if (b.sha256 && (await sha256hex(out)) !== b.sha256) throw new Error("копия повреждена при передаче (sha256 не совпадает)");
  const a = h("a", { href: URL.createObjectURL(new Blob([out], { type: "application/octet-stream" })), download: b.name === "last" ? "pns-backup-last.bin" : `pns-${b.name.replace(/\.gz$/, "")}` });
  document.body.append(a);
  a.click();
  setTimeout(() => {
    URL.revokeObjectURL(a.href);
    a.remove();
  }, 1000);
}

function chipInfo(chip) {
  if (!chip) return h("div", { class: "muted" }, "Флеш ПНС ещё не читался: версия неизвестна. Она читается при загрузке BMC, если платформа выключена.");
  const i = chip.info || {};
  const d = i.dtb || {};
  return h(
    "div",
    {},
    kv([
      ["Версия ПНС", i.boot || (i.empty ? "флеш пуст" : "не найдена")],
      ["Плата (сборка)", i.board],
      ["VBIOS", i.vbios],
      ["Device tree", d.valid ? `${d.version || "без версии"} (${d.model || "?"})` : d.error],
      ["Совместимость", d.compatible],
      ["Прочитано", `${when(chip.time)}, ${SOURCES[chip.source] || chip.source}`],
    ]),
  );
}

function checkInfo(c) {
  if (!c) return null;
  const i = c.image || {};
  const d = i.dtb || {};
  return h(
    "div",
    { class: "section" },
    kv([
      ["Версия ПНС", i.boot],
      ["Плата (сборка)", i.board],
      ["VBIOS", i.vbios],
      ["Device tree", d.valid ? `${d.version} (${d.model}, ${d.compatible})` : "нет (не меняется)"],
      ["Записывается", c.covers ? `${hex(c.covers[0])}–${hex(c.covers[1])} (${fmtBytes(c.covers[1])}), остальное не меняется` : undefined],
    ]),
    ...(c.errors || []).map((e) => h("div", { class: "alert err" }, e)),
    ...(c.warnings || []).map((e) => h("div", { class: "alert warn" }, e)),
  );
}

export function pnsFlashCard() {
  const card = h("div", { class: "card span2", id: "pns-flash" }, h("h3", {}, "Прошивка ПНС"), h("div", { class: "muted" }, "Загрузка…"));
  let timer = null;
  let alive = true;
  let busy = false; // an upload or a download in this page
  let check = null; // the last check of the uploaded image
  let last = null;
  const bar = h("div", { class: "progress hidden" }, h("div", {}));
  const barText = h("div", { class: "muted small" });

  async function refresh() {
    clearTimeout(timer);
    if (!alive) return;
    let st;
    try {
      st = await status();
    } catch (e) {
      if (!alive) return;
      if (!last) card.replaceChildren(h("h3", {}, "Прошивка ПНС"), h("div", { class: "muted" }, `Сервис reimu-pnsd недоступен: ${e.message}`));
      timer = setTimeout(refresh, 5000);
      return;
    }
    if (!alive) return;
    const wasRunning = last && last.state && last.state.state === "running";
    last = st;
    if (!busy) card.replaceChildren(...render(st));
    const running = st.state && st.state.state === "running" && !st.stale;
    if (running) timer = setTimeout(refresh, 1500);
    else if (wasRunning) timer = setTimeout(refresh, 1000);
    else timer = setTimeout(refresh, 15000);
  }

  function setBar(p, text) {
    bar.classList.remove("hidden");
    bar.firstChild.style.width = `${Math.round(p * 100)}%`;
    barText.textContent = text;
  }

  async function act(action, arg, question) {
    if (question && !confirm(question)) return;
    try {
      const r = await run(action, arg);
      toast(r.message, r.ok ? "ok" : "err");
    } catch (e) {
      toast(`Ошибка: ${e.message}`, "err");
    }
    setTimeout(refresh, 700);
  }

  function render(st) {
    const out = [h("h3", {}, "Прошивка ПНС")];
    if (!st.tool) {
      out.push(h("p", { class: "muted" }, "Эта прошивка BMC не умеет записывать флеш ПНС (нет reimu-pns-flash)."));
      return out;
    }
    const job = st.state;
    const running = job && job.state === "running" && !st.stale;
    const on = hostOn();
    out.push(chipInfo(st.chip));
    if (on) out.push(h("div", { class: "alert warn" }, "Платформа включена: флеш ПНС занят хостом. Чтение и запись возможны только при выключенной платформе."));

    if (job) {
      const [label, cls] = st.stale ? STATES.interrupted : STATES[job.state] || [job.state, ""];
      out.push(
        h(
          "div",
          { class: "section" },
          h("div", { class: "small" }, `${JOBS[job.job] || job.job}: `, h("span", { class: `badge ${cls}` }, label), " ", h("span", { class: "muted" }, when(job.finished || job.started))),
          h("div", { class: "small" }, job.message),
          job.warning ? h("div", { class: "alert warn" }, job.warning) : null,
          st.stale ? h("div", { class: "alert err" }, "Операция прервалась, не завершившись. Если шла запись, запишите обратно резервную копию (ниже) до включения платформы.") : null,
          running ? h("div", { class: "progress" }, h("div", { style: { width: `${job.progress || 0}%` } })) : null,
          running ? h("div", { class: "muted small" }, `${job.progress || 0}%: включение платформы заблокировано до конца операции`) : null,
        ),
      );
    }

    // backups
    const backups = [...(st.backups || [])];
    if (st.kept) backups.push({ ...st.kept, kept: true });
    const read = h("button", { class: "btn", disabled: running || on || busy }, "Прочитать флеш (резервная копия)");
    read.addEventListener("click", () => act("read", "", "Прочитать весь флеш ПНС в резервную копию (в память BMC)? Платформа должна оставаться выключенной: на время чтения (около минуты) её включение блокируется."));
    out.push(
      h(
        "div",
        { class: "section" },
        h("h3", {}, "Резервные копии"),
        backups.length
          ? h(
              "table",
              { class: "tbl" },
              h(
                "tbody",
                {},
                backups.map((b) => {
                  const dl = h("button", { class: "btn small", disabled: busy }, "Скачать");
                  dl.addEventListener("click", async () => {
                    busy = true;
                    dl.disabled = true;
                    try {
                      await downloadBackup(b, (p) => setBar(p, `Скачивание ${b.name}: ${Math.round(p * 100)}%`));
                      toast("Резервная копия скачана и сверена (sha256)", "ok");
                    } catch (e) {
                      toast(`Скачивание не удалось: ${e.message}`, "err");
                    } finally {
                      busy = false;
                      bar.classList.add("hidden");
                      barText.textContent = "";
                      refresh();
                    }
                  });
                  const wr = h("button", { class: "btn small danger", disabled: running || on || busy }, "Записать обратно");
                  wr.addEventListener("click", () =>
                    act("write", b.name, `Записать во флеш ПНС резервную копию ${b.kept ? "(сделанную перед последней записью)" : b.name} от ${when(b.time)}?\n\nФлеш будет сначала прочитан (новая копия), затем записаны только отличающиеся блоки, и весь флеш сверен с копией.`),
                  );
                  return h(
                    "tr",
                    {},
                    h("td", {}, b.kept ? "до последней записи (хранится на флеше BMC)" : b.name),
                    h("td", { class: "muted" }, when(b.time)),
                    h("td", { class: "mono small muted" }, b.sha256 ? `${b.sha256.slice(0, 12)}…` : ""),
                    h("td", { class: "row end" }, dl, wr),
                  );
                }),
              ),
            )
          : h("div", { class: "muted small" }, "Нет: копия делается перед каждой записью, или кнопкой ниже. Копии в памяти BMC пропадают при его перезагрузке — скачайте нужную."),
        h("div", { class: "row" }, read),
      ),
    );

    // a new image
    const file = h("input", { type: "file", accept: ".bin,.rom,.img,application/octet-stream", disabled: running || busy });
    const send = h("button", { class: "btn", disabled: true }, "Загрузить и проверить");
    file.addEventListener("change", () => (send.disabled = !file.files.length));
    send.addEventListener("click", async () => {
      const f = file.files[0];
      if (!f) return;
      if (f.size > 64 * 1024 * 1024) return toast("Файл больше флеша ПНС (64 МиБ)", "err");
      const head = new Uint8Array(await f.slice(0, 8).arrayBuffer());
      if (!MAGIC.every((x, i) => head[i] === x)) return toast("Это не образ ПНС: нет заголовка загрузчика Lintel в начале файла", "err");
      busy = true;
      send.disabled = file.disabled = true;
      try {
        const r = await uploadImage(f, (p) => setBar(p, `Загрузка ${f.name}: ${Math.round(p * 100)}%`));
        check = { ...(r.check || {}), name: f.name, size: f.size, ok: r.ok, message: r.message };
        toast(r.message, r.ok ? "ok" : "err");
      } catch (e) {
        toast(`Загрузка не удалась: ${e.message}`, "err");
      } finally {
        busy = false;
        bar.classList.add("hidden");
        barText.textContent = "";
        refresh();
      }
    });
    const up = st.upload;
    const section = h("div", { class: "section" }, h("h3", {}, "Новый образ"), h("div", { class: "row" }, file, send), bar, barText);
    if (up) {
      const c = check && check.size === up.size ? check : null;
      const write = h("button", { class: "btn primary", disabled: running || on || busy || !(c && c.ok) }, "Записать во флеш ПНС");
      write.addEventListener("click", () => {
        const i = (c && c.image) || {};
        act(
          "write",
          "upload",
          `Записать во флеш ПНС образ ${c.name} (${fmtBytes(up.size)})?\n\n${i.boot || ""}\nDevice tree: ${(i.dtb && i.dtb.version) || "не меняется"}\n\n` +
            "Перед записью весь флеш будет прочитан в резервную копию. Пишутся только отличающиеся блоки, каждый читается обратно; затем весь флеш сверяется. Платформу до конца записи (несколько минут) включить нельзя.",
        );
      });
      const recheck = h("button", { class: "btn", disabled: busy }, "Проверить");
      recheck.addEventListener("click", async () => {
        try {
          const r = await run("check");
          check = { ...(r.check || {}), name: (check && check.name) || "загруженный образ", size: up.size, ok: r.ok, message: r.message };
          card.replaceChildren(...render(last));
        } catch (e) {
          toast(`Ошибка: ${e.message}`, "err");
        }
      });
      const drop = h("button", { class: "btn small", disabled: running || busy }, "Удалить");
      drop.addEventListener("click", () => {
        check = null;
        act("discard");
      });
      section.append(
        h("div", { class: "small" }, `Загружен: ${(c && c.name) || "образ"}, ${fmtBytes(up.size)}, ${when(up.time)} `, c ? h("span", { class: `badge ${c.ok ? "ok" : "err"}` }, c.ok ? "годен" : "не годен") : h("span", { class: "badge" }, "не проверен")),
        c ? checkInfo(c) : null,
        h("div", { class: "row" }, c ? null : recheck, write, drop),
      );
    }
    section.append(h("p", { class: "muted small" }, "Из консоли BMC: reimu-pns-flash check|write ФАЙЛ, reimu-pns-flash read; update-host-flash URL."));
    out.push(section);
    return out;
  }

  const onPower = () => last && !busy && card.replaceChildren(...render(last));
  document.addEventListener("reimu-power", onPower);
  refresh();
  card.stop = () => {
    alive = false;
    clearTimeout(timer);
    document.removeEventListener("reimu-power", onPower);
  };
  return card;
}
