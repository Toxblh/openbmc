// The web UI's view of the BMC (reimu-pnsd view/live, view/static, read by
// js/lib/view.js, the real file) against bmcweb on the same BMC in QEMU: for
// every URL the view answers, what it says = what bmcweb says (the fields the
// BMC fills); and one view request against bmcweb's many.
//
//     BASE=https://127.0.0.1:PORT WWW=<web tree> node view.qemu.mjs
// (tests/ab64/test_fru_sensors_qemu.py runs it with the platform on)
import path from "node:path";
import { pathToFileURL } from "node:url";

process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0";
const BASE = process.env.BASE;
const WWW = process.env.WWW;
const { fromView } = await import(pathToFileURL(path.join(WWW, "js/lib/view.js")).href);
const AUTH = { Authorization: "Basic " + Buffer.from("root:0penBmc").toString("base64"), Accept: "application/json" };

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };
async function get(url) {
  const t = performance.now();
  const r = await fetch(BASE + url, { headers: AUTH });
  const body = r.ok ? await r.json() : null;
  return { status: r.status, body, ms: performance.now() - t };
}

const vl = await get("/xyz/openbmc_project/reimu/view/live");
const vs = await get("/xyz/openbmc_project/reimu/view/static");
check(vl.status === 200 && vs.status === 200, `the view is there (${vl.status}, ${vs.status})`);
const live = JSON.parse(vl.body.data.Json), stat = JSON.parse(vs.body.data.Json);
check(live.static === stat.hash, `the live document names the static one's version (${live.static})`);

const near = (a, b) => typeof a === "number" && typeof b === "number" ? Math.abs(a - b) <= Math.max(2, Math.abs(b) * 0.1) : a === b;
const same = (a, b, keys, what) => {
  const diff = keys.filter((k) => JSON.stringify(a && a[k]) !== JSON.stringify(b && b[k]));
  check(!diff.length, `${what}: ${keys.join(", ")} as bmcweb's${diff.length ? ` (differ: ${diff.map((k) => `${k} ${JSON.stringify(a && a[k])} != ${JSON.stringify(b && b[k])}`).join("; ")})` : ""}`);
};

let rfMs = 0;
const rf = async (url) => { const r = await get(url); rfMs += r.ms; return r.body; };

same(fromView("/redfish/v1/Systems/system", live, stat), await rf("/redfish/v1/Systems/system"), ["PowerState"], "Systems/system");
const col = fromView("/redfish/v1/Chassis", live, stat), rcol = await rf("/redfish/v1/Chassis");
same(col, rcol, ["Members"], "Chassis");
for (const m of (rcol && rcol.Members) || []) {
  const u = m["@odata.id"];
  same(fromView(u, live, stat), await rf(u), ["Id", "ChassisType", "Manufacturer", "Model", "PartNumber", "SerialNumber", "PowerState"], u);
  const vt = fromView(`${u}/Thermal`, live, stat), rt = await rf(`${u}/Thermal`);
  for (const [k, v] of [["Temperatures", "ReadingCelsius"], ["Fans", "Reading"]]) {
    const names = (x) => ((x && x[k]) || []).map((s) => s.Name).sort();
    same({ n: names(vt) }, { n: names(rt) }, ["n"], `${u}/Thermal ${k} (names)`);
    for (const s of (rt && rt[k]) || []) {
      const mine = ((vt && vt[k]) || []).find((x) => x.Name === s.Name);
      check(mine && near(mine[v], s[v]) && mine.Status.Health === s.Status.Health && mine.ReadingUnits === s.ReadingUnits &&
        ["UpperThresholdNonCritical", "UpperThresholdCritical", "LowerThresholdNonCritical", "LowerThresholdCritical"].every((t) => mine[t] === s[t]),
        `${s.Name}: ${mine && mine[v]} ~ ${s[v]}, health, units, thresholds as bmcweb's`);
    }
  }
  const vp = fromView(`${u}/Power`, live, stat), rp = await rf(`${u}/Power`);
  const vn = (x) => ((x && x.Voltages) || []).map((s) => s.Name).sort();
  same({ n: vn(vp) }, { n: vn(rp) }, ["n"], `${u}/Power Voltages (names)`);
}
same(fromView("/redfish/v1/Managers/bmc", live, stat), await rf("/redfish/v1/Managers/bmc"), ["FirmwareVersion", "UUID", "Model", "ManagerType"], "Managers/bmc");
const ve = fromView("/redfish/v1/Managers/bmc/EthernetInterfaces/eth0", live, stat), re = await rf("/redfish/v1/Managers/bmc/EthernetInterfaces/eth0");
same(ve, re, ["MACAddress", "LinkStatus"], "eth0");
same({ ip: ((ve && ve.IPv4Addresses) || []).map((a) => a.Address) }, { ip: ((re && re.IPv4Addresses) || []).map((a) => a.Address) }, ["ip"], "eth0 IPv4");
for (const p of ["/xyz/openbmc_project/reimu/sensors", "/xyz/openbmc_project/reimu/fans", "/xyz/openbmc_project/reimu/hostlink",
  "/xyz/openbmc_project/reimu/overheat", "/xyz/openbmc_project/reimu/pns", "/xyz/openbmc_project/led/groups/enclosure_identify"]) {
  const mine = fromView(p, live, stat), theirs = await rf(p);
  const keys = (x) => Object.keys((x && x.data) || {}).sort().join(",");
  const top = (x) => { try { return Object.keys(JSON.parse(x.data.Json)).sort().join(","); } catch (e) { return JSON.stringify(x && x.data); } };
  check(mine && theirs && keys(mine) === keys(theirs) && (mine.data.Json === undefined || top(mine) === top(theirs)), `${p}: the same properties and document fields`);
}
console.log(`  the view: ${Math.round(vl.ms + vs.ms)} ms in 2 requests; bmcweb: ${Math.round(rfMs)} ms in its requests, one after another`);
console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
