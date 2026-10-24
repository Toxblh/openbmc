// The host agent's self-contained installer (the «Хост» page's «Скачать
// установщик»): the BMC's /agent/install.sh with the pairing string written
// into REIMU_PAIR_DEFAULT and the agent's files appended, one line each
// ("NAME BASE64") after the line #__REIMU_PAYLOAD__. install.sh checks them
// against the SHA-256 written into it at the BMC's build, as it checks
// downloaded ones; the shell never reads past install.sh's own block.
export const AGENT_FILES = ["reimu-hostd.py", "reimu-hostd.service", "reimu-hostd.conf"];
const PAIR_RE = /^reimu1:[A-Za-z0-9_-]{16,2000}$/;
const SLOT = "\nREIMU_PAIR_DEFAULT=''\n";

function b64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

// installSh: install.sh's text, files: {name: Uint8Array}
export function packInstaller(installSh, files, pair) {
  if (!PAIR_RE.test(pair || "")) throw new Error("не строка привязки");
  if (!installSh.includes(SLOT)) throw new Error("install.sh без REIMU_PAIR_DEFAULT");
  let out = installSh.replace(SLOT, `\nREIMU_PAIR_DEFAULT='${pair}'\n`);
  if (!out.endsWith("\n")) out += "\n";
  out += "#__REIMU_PAYLOAD__\n";
  for (const f of AGENT_FILES) {
    if (!files[f]) throw new Error(`нет ${f}`);
    out += `${f} ${b64(files[f])}\n`;
  }
  return out;
}

export async function buildInstaller(pair, base = "/agent/") {
  const get = async (f) => {
    const r = await fetch(base + f, { cache: "no-store", credentials: "same-origin" });
    if (!r.ok) throw new Error(`${base}${f}: HTTP ${r.status}`);
    return new Uint8Array(await r.arrayBuffer());
  };
  const sh = new TextDecoder().decode(await get("install.sh"));
  const files = {};
  for (const f of AGENT_FILES) files[f] = await get(f);
  return packInstaller(sh, files, pair);
}
