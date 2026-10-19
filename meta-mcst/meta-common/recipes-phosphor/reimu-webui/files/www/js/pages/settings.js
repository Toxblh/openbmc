import { api, h, toast } from "../api.js";

let root;

const field = (label, input) => h("label", {}, label, input);

export async function mount(el) {
  root = el;
  el.append(h("h2", {}, "Настройки BMC"), h("div", { class: "grid", id: "set-grid" }, h("div", { class: "card" }, "Загрузка…")));
  await render();
}

async function render() {
  const [mgr, proto, ifaces, accounts] = await Promise.all([
    api.get("/redfish/v1/Managers/bmc").catch(() => null),
    api.get("/redfish/v1/Managers/bmc/NetworkProtocol").catch(() => null),
    api.collection("/redfish/v1/Managers/bmc/EthernetInterfaces").catch(() => []),
    api.collection("/redfish/v1/AccountService/Accounts").catch(() => []),
  ]);
  if (!root) return;
  root.querySelector("#set-grid").replaceChildren(networkCard(proto, ifaces), timeCard(mgr, proto), usersCard(accounts), bmcCard(mgr));
}

function networkCard(proto, ifaces) {
  const host = h("input", { value: (proto && proto.HostName) || "" });
  const saveHost = h("button", { class: "btn primary" }, "Сохранить имя");
  saveHost.addEventListener("click", () => patch("/redfish/v1/Managers/bmc/NetworkProtocol", { HostName: host.value.trim() }, "Имя хоста сохранено"));
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Сеть"),
    field("Имя хоста", host),
    h("div", { class: "row end" }, saveHost),
    ...ifaces.map((i) => {
      const dhcp = h("input", { type: "checkbox", checked: !!(i.DHCPv4 && i.DHCPv4.DHCPEnabled) });
      const st = (i.IPv4StaticAddresses && i.IPv4StaticAddresses[0]) || (i.IPv4Addresses && i.IPv4Addresses[0]) || {};
      const addr = h("input", { value: st.Address || "", placeholder: "192.168.0.10" });
      const mask = h("input", { value: st.SubnetMask || "", placeholder: "255.255.255.0" });
      const gw = h("input", { value: st.Gateway || "", placeholder: "192.168.0.1" });
      const save = h("button", { class: "btn primary" }, `Применить ${i.Id}`);
      const sync = () => [addr, mask, gw].forEach((x) => (x.disabled = dhcp.checked));
      dhcp.addEventListener("change", sync);
      sync();
      save.addEventListener("click", () => {
        if (!confirm(`Изменить настройки ${i.Id}? Связь с BMC может прерваться.`)) return;
        const body = dhcp.checked ? { DHCPv4: { DHCPEnabled: true } } : { DHCPv4: { DHCPEnabled: false }, IPv4StaticAddresses: [{ Address: addr.value.trim(), SubnetMask: mask.value.trim(), Gateway: gw.value.trim() }] };
        patch(i["@odata.id"], body, `Настройки ${i.Id} применены`);
      });
      const cur = (i.IPv4Addresses || []).map((a) => `${a.Address}/${a.SubnetMask} (${a.AddressOrigin})`).join(", ");
      return h("div", { class: "section" }, h("h3", {}, `${i.Id} `, h("span", { class: "muted small mono" }, i.MACAddress || "")), h("div", { class: "muted small" }, `Сейчас: ${cur || "нет адреса"}`), h("label", { class: "inline" }, dhcp, "DHCP"), field("IPv4-адрес", addr), field("Маска", mask), field("Шлюз", gw), h("div", { class: "row end" }, save));
    }),
  );
}

function timeCard(mgr, proto) {
  const ntp = (proto && proto.NTP) || {};
  const enabled = h("input", { type: "checkbox", checked: !!ntp.ProtocolEnabled });
  const servers = h("input", { value: (ntp.NTPServers || []).filter(Boolean).join(", "), placeholder: "pool.ntp.org" });
  const save = h("button", { class: "btn primary" }, "Сохранить NTP");
  save.addEventListener("click", () =>
    patch("/redfish/v1/Managers/bmc/NetworkProtocol", { NTP: { ProtocolEnabled: enabled.checked, NTPServers: servers.value.split(/[\s,]+/).filter(Boolean) } }, "Настройки NTP сохранены"),
  );
  const setNow = h("button", { class: "btn" }, "Установить время браузера");
  setNow.addEventListener("click", () => {
    const d = new Date();
    const off = -d.getTimezoneOffset();
    const pad = (n) => String(Math.abs(n)).padStart(2, "0");
    const iso = `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}${off >= 0 ? "+" : "-"}${pad(Math.trunc(off / 60))}:${pad(off % 60)}`;
    patch("/redfish/v1/Managers/bmc", { DateTime: iso }, "Время BMC установлено");
  });
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Время"),
    h("dl", { class: "kv" }, h("dt", {}, "Время BMC"), h("dd", {}, mgr && mgr.DateTime ? mgr.DateTime.replace("T", " ") : "—"), h("dt", {}, "Браузер"), h("dd", {}, new Date().toString())),
    h("label", { class: "inline" }, enabled, "синхронизация по NTP"),
    field("NTP-серверы", servers),
    h("div", { class: "row end" }, setNow, save),
  );
}

function usersCard(accounts) {
  return h(
    "div",
    { class: "card" },
    h("h3", {}, "Пользователи"),
    h(
      "table",
      { class: "tbl" },
      h("tbody", {}, accounts.map((a) => {
        const btn = h("button", { class: "btn small" }, "Сменить пароль");
        btn.addEventListener("click", () => {
          const p1 = prompt(`Новый пароль для ${a.UserName}:`);
          if (!p1) return;
          if (prompt("Повторите пароль:") !== p1) return toast("Пароли не совпадают", "err");
          patch(a["@odata.id"], { Password: p1 }, "Пароль изменён");
        });
        return h("tr", {}, h("td", {}, a.UserName), h("td", { class: "muted" }, a.RoleId), h("td", {}, a.Enabled ? "" : h("span", { class: "badge" }, "отключён")), h("td", { class: "row end" }, btn));
      })),
    ),
    h("p", { class: "muted small" }, "Пароль VNC для KVM меняется командой vncpasswd в консоли BMC."),
  );
}

function bmcCard(mgr) {
  const reboot = h("button", { class: "btn danger" }, "Перезагрузить BMC");
  reboot.addEventListener("click", async () => {
    if (!confirm("Перезагрузить BMC? Платформа продолжит работать, веб-интерфейс будет недоступен около полутора минут.")) return;
    try {
      await api.post("/redfish/v1/Managers/bmc/Actions/Manager.Reset", { ResetType: "GracefulRestart" });
      toast("BMC перезагружается", "ok");
    } catch (e) {
      toast(e.message, "err");
    }
  });
  return h("div", { class: "card" }, h("h3", {}, "BMC"), h("dl", { class: "kv" }, h("dt", {}, "Прошивка"), h("dd", {}, (mgr && mgr.FirmwareVersion) || "—")), h("p", { class: "muted small" }, "Обновление прошивки BMC и ПНС выполняется утилитами firmware-updater и update-host-flash (см. руководство REIMU)."), h("div", { class: "row" }, reboot));
}

async function patch(url, body, okText) {
  try {
    await api.patch(url, body);
    toast(okText, "ok");
    setTimeout(render, 1000);
  } catch (e) {
    toast(`Ошибка: ${e.message}`, "err");
  }
}

export function unmount() {
  root = null;
}
