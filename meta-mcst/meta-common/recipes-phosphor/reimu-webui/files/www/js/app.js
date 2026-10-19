import { api, toast } from "./api.js";

const pages = {
  overview: () => import("./pages/overview.js"),
  kvm: () => import("./pages/kvm.js"),
  console: () => import("./pages/console.js"),
  media: () => import("./pages/media.js"),
  sensors: () => import("./pages/sensors.js"),
  hardware: () => import("./pages/hardware.js"),
  logs: () => import("./pages/journal.js"),
  settings: () => import("./pages/settings.js"),
};

// Pages that keep running while hidden (an active virtual media session must
// survive navigation, otherwise the host loses its disk).
const persistent = new Set(["media"]);

const state = { current: null, mounted: {}, loggedIn: false, powerTimer: null };
export const appState = { power: null, user: "" };

const $ = (id) => document.getElementById(id);

function showLogin(msg = "") {
  state.loggedIn = false;
  clearInterval(state.powerTimer);
  $("app").classList.add("hidden");
  $("login").classList.remove("hidden");
  $("login-error").textContent = msg;
  $("login-host").textContent = location.host;
  $("login-pass").focus();
}

async function showApp(user) {
  state.loggedIn = true;
  appState.user = user || appState.user || "root";
  $("user-btn").textContent = `${appState.user} ▾`;
  $("nav-logout").textContent = `Выйти (${appState.user})`;
  $("login").classList.add("hidden");
  $("app").classList.remove("hidden");
  pollPower();
  clearInterval(state.powerTimer);
  state.powerTimer = setInterval(pollPower, 5000);
  route();
}

async function pollPower() {
  try {
    const sys = await api.get("/redfish/v1/Systems/system", { noAuthEvent: false });
    appState.power = sys.PowerState;
    const led = $("power-led");
    const on = sys.PowerState === "On";
    led.className = "led " + (on ? "on" : sys.PowerState === "Off" ? "off" : "warn");
    $("power-text").textContent = { On: "Включена", Off: "Выключена" }[sys.PowerState] || sys.PowerState || "?";
    document.dispatchEvent(new CustomEvent("reimu-power", { detail: sys }));
  } catch (e) {
    $("power-led").className = "led err";
    $("power-text").textContent = "нет связи";
  }
}

export async function powerAction(kind) {
  const names = {
    On: "включить",
    GracefulShutdown: "выключить (ACPI)",
    ForceOff: "ПРИНУДИТЕЛЬНО выключить",
    GracefulRestart: "перезагрузить (ACPI)",
    ForceRestart: "сбросить (RESET)",
  };
  if (!confirm(`Действительно ${names[kind] || kind} платформу?`)) return;
  try {
    await api.post("/redfish/v1/Systems/system/Actions/ComputerSystem.Reset", { ResetType: kind });
    toast(`Команда «${names[kind]}» отправлена`, "ok");
    setTimeout(pollPower, 1500);
  } catch (e) {
    toast(`Ошибка: ${e.message}`, "err");
  }
}

// Navigation is serialized: page modules keep per-page state, so a page
// must never be mounted twice concurrently (e.g. login + hashchange).
let routing = Promise.resolve();
function route() {
  routing = routing.then(doRoute, doRoute);
  return routing;
}

async function doRoute() {
  if (!state.loggedIn) return;
  const name = (location.hash.replace(/^#\/?/, "").split(/[/?]/)[0] || "overview").toLowerCase();
  const key = pages[name] ? name : "overview";
  document.querySelectorAll("#nav a").forEach((a) => a.classList.toggle("active", a.dataset.page === key));
  const navLink = document.querySelector(`#nav a[data-page="${key}"]`);
  $("page-title").textContent = navLink ? navLink.textContent : "";
  setNavOpen(false);
  document.querySelectorAll(".dropdown.open").forEach((d) => d.classList.remove("open"));
  $("page").querySelectorAll(".load-error").forEach((e) => e.remove());

  if (state.current && state.current !== key) {
    const cur = state.mounted[state.current];
    if (cur) {
      if (persistent.has(state.current)) {
        cur.el.classList.add("hidden");
        cur.mod.hide && cur.mod.hide();
      } else {
        cur.mod.unmount && cur.mod.unmount();
        cur.el.remove();
        delete state.mounted[state.current];
      }
    }
  }
  state.current = key;
  const pageRoot = $("page");
  let entry = state.mounted[key];
  if (!entry) {
    const el = document.createElement("div");
    el.className = "page-view";
    pageRoot.append(el);
    let mod;
    try {
      mod = await pages[key]();
    } catch (e) {
      // not kept in state.mounted: the next navigation retries the import
      el.className = "page-view load-error";
      el.textContent =
        `Не удалось загрузить страницу: ${e.message}. ` +
        "Если в консоли браузера ERR_BLOCKED_BY_CLIENT, файл блокирует расширение (блокировщик рекламы): отключите его для этого адреса.";
      state.current = null;
      return;
    }
    entry = state.mounted[key] = { mod, el };
    try {
      await mod.mount(el);
    } catch (e) {
      el.textContent = `Ошибка загрузки страницы: ${e.message}`;
    }
  } else {
    entry.el.classList.remove("hidden");
    entry.mod.show && entry.mod.show();
  }
  pageRoot.classList.toggle("fill", !!entry.mod.fill);
  entry.el.style.height = entry.mod.fill ? "100%" : "";
  entry.el.style.display = entry.mod.fill ? "flex" : "";
  entry.el.style.flexDirection = entry.mod.fill ? "column" : "";
}

// Narrow screens: the page links live in a drawer opened by the ☰ button
function setNavOpen(open) {
  document.body.classList.toggle("nav-open", open);
  $("nav-btn").setAttribute("aria-expanded", String(open));
  $("nav-btn").textContent = open ? "✕" : "☰";
}

function setupDropdowns() {
  $("nav-btn").addEventListener("click", (e) => {
    e.stopPropagation();
    setNavOpen(!document.body.classList.contains("nav-open"));
  });
  $("nav").addEventListener("click", (e) => e.target.closest("a") && setNavOpen(false));
  $("nav-logout").addEventListener("click", (e) => {
    e.preventDefault();
    $("logout-btn").click();
  });
  document.addEventListener("click", (e) => {
    const dd = e.target.closest(".dropdown");
    if (dd && e.target === dd) {
      dd.classList.remove("open"); // the backdrop beside the open menu
      return;
    }
    document.querySelectorAll(".dropdown.open").forEach((d) => d !== dd && d.classList.remove("open"));
    if (dd && e.target.closest(".btn") && e.target.closest(".dropdown") === dd && !e.target.closest(".dropdown-menu")) {
      dd.classList.toggle("open");
    } else if (e.target.closest(".dropdown-menu")) {
      dd && dd.classList.remove("open");
    }
  });
  // the backdrop beside an open menu, on touch screens without waiting for a click
  document.addEventListener(
    "touchend",
    (e) => {
      const dd = e.target.closest && e.target.closest(".dropdown.open");
      if (dd && e.target === dd) {
        e.preventDefault();
        dd.classList.remove("open");
      }
    },
    { passive: false },
  );
  $("power-menu").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-reset]");
    if (b) powerAction(b.dataset.reset);
  });
  $("logout-btn").addEventListener("click", async () => {
    await api.logout();
    location.hash = "#/";
    location.reload();
  });
}

async function init() {
  setupDropdowns();
  api.on("unauthorized", () => {
    if (state.loggedIn) showLogin("Сессия истекла, войдите снова");
  });
  $("login-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const user = $("login-user").value.trim();
    $("login-error").textContent = "";
    try {
      await api.login(user, $("login-pass").value);
      $("login-pass").value = "";
      try {
        sessionStorage.setItem("reimu-user", user);
      } catch (err) {
        /* storage may be unavailable */
      }
      showApp(user);
    } catch (err) {
      $("login-error").textContent = err.status === 401 ? "Неверный логин или пароль" : `Ошибка: ${err.message}`;
    }
  });
  window.addEventListener("hashchange", route);

  let user = "";
  try {
    user = sessionStorage.getItem("reimu-user") || "";
  } catch (e) {
    /* ignore */
  }
  try {
    await api.whoami();
    showApp(user);
  } catch (e) {
    showLogin();
  }
}

init();
