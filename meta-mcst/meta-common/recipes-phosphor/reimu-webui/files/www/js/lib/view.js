// The BMC's view (reimu-hwmon: /xyz/openbmc_project/reimu/view/live; reimu-pnsd:
// /static): what the pages read, gathered on the BMC into one answer instead
// of 10-20 requests through bmcweb. The pages keep asking for the same URLs:
// api.get answers those the view has from it, in the shape bmcweb gives them
// (its REST for D-Bus objects, Redfish for the rest; only the fields of
// Redfish the BMC fills, by bmcweb's rules). Anything else goes to bmcweb.

const SENS = "/xyz/openbmc_project/sensors/";
const INV = "/xyz/openbmc_project/inventory/";
const RF = "/redfish/v1";
const base = (p) => p.slice(p.lastIndexOf("/") + 1);
const tail = (s) => (typeof s === "string" ? s.slice(s.lastIndexOf(".") + 1) : undefined);
const OK = () => ({ State: "Enabled", Health: "OK", HealthRollup: "OK" });

// the chassis bmcweb lists: inventory items that are a board or a chassis
function chassisOf(stat) {
  const out = [];
  for (const [path, ifs] of Object.entries(stat.inventory || {}))
    if (ifs["xyz.openbmc_project.Inventory.Item.Board"] || ifs["xyz.openbmc_project.Inventory.Item.Chassis"]) out.push([path, ifs]);
  return out;
}

// one sensor as bmcweb's Thermal and Power give it
function sensor(path, ifs, valueKey) {
  const v = ifs["xyz.openbmc_project.Sensor.Value"] || {};
  const w = ifs["xyz.openbmc_project.Sensor.Threshold.Warning"] || {};
  const c = ifs["xyz.openbmc_project.Sensor.Threshold.Critical"] || {};
  const k = 10 ** (typeof v.Scale === "number" ? v.Scale : 0);
  const sc = (x) => (typeof x === "number" ? x * k : undefined);
  const health = c.CriticalAlarmHigh || c.CriticalAlarmLow ? "Critical" : w.WarningAlarmHigh || w.WarningAlarmLow ? "Warning" : "OK";
  const name = base(path);
  const s = { MemberId: name, Name: name.replace(/_/g, " "), Status: { State: "Enabled", Health: health }, [valueKey]: sc(v.Value) ?? null };
  const th = { UpperThresholdNonCritical: w.WarningHigh, LowerThresholdNonCritical: w.WarningLow, UpperThresholdCritical: c.CriticalHigh, LowerThresholdCritical: c.CriticalLow };
  for (const [key, x] of Object.entries(th)) if (typeof x === "number") s[key] = sc(x);
  return s;
}

function sensorsOf(live, type, valueKey, extra = {}) {
  return Object.entries(live.sensors || {})
    .filter(([p]) => p.startsWith(SENS + type + "/"))
    .map(([p, ifs]) => ({ ...sensor(p, ifs, valueKey), ...extra }))
    .sort((a, b) => (a.Name < b.Name ? -1 : a.Name > b.Name ? 1 : 0));
}

// the answer the view has for URL, or undefined
export function fromView(url, live, stat) {
  const obj = (live.objects && live.objects[url]) || (stat.objects && stat.objects[url]);
  if (obj) return { data: { ...obj }, message: "200 OK", status: "ok" };
  if (url === `${RF}/Systems/system`) {
    // the memory: bmcweb's sum of the DIMMs in the inventory, each in whole GiB
    let gib = 0;
    for (const ifs of Object.values(stat.inventory || {})) {
      const d = ifs["xyz.openbmc_project.Inventory.Item.Dimm"];
      if (d && typeof d.MemorySizeInKB === "number") gib += Math.floor(d.MemorySizeInKB / (1024 * 1024));
    }
    return { "@odata.id": url, Id: "system", Name: "system", PowerState: live.power, Status: { ...OK(), State: live.power === "On" ? "Enabled" : "Disabled" }, MemorySummary: { TotalSystemMemoryGiB: gib } };
  }
  if (url === `${RF}/Chassis`) return { "@odata.id": url, Members: chassisOf(stat).map(([p]) => ({ "@odata.id": `${RF}/Chassis/${base(p)}` })) };
  const ch = /^\/redfish\/v1\/Chassis\/([^/]+)(\/Thermal|\/Power)?$/.exec(url);
  if (ch) {
    const found = chassisOf(stat).find(([p]) => base(p) === ch[1]);
    if (!found) return undefined;
    const id = `${RF}/Chassis/${ch[1]}`;
    if (ch[2] === "/Thermal")
      return { "@odata.id": url, Id: "Thermal", Name: "Thermal", Temperatures: sensorsOf(live, "temperature", "ReadingCelsius"), Fans: [...sensorsOf(live, "fan_tach", "Reading", { ReadingUnits: "RPM" }), ...sensorsOf(live, "fan_pwm", "Reading", { ReadingUnits: "Percent" })] };
    if (ch[2] === "/Power") return { "@odata.id": url, Id: "Power", Name: "Power", Voltages: sensorsOf(live, "voltage", "ReadingVolts") };
    const ifs = found[1];
    const a = ifs["xyz.openbmc_project.Inventory.Decorator.Asset"] || {};
    const c = ifs["xyz.openbmc_project.Inventory.Item.Chassis"] || {};
    return {
      "@odata.id": id, Id: ch[1], Name: ch[1], ChassisType: tail(c.Type) || "RackMount", Manufacturer: a.Manufacturer, Model: a.Model, PartNumber: a.PartNumber, SerialNumber: a.SerialNumber,
      PowerState: live.power, Status: OK(), Thermal: { "@odata.id": `${id}/Thermal` }, Power: { "@odata.id": `${id}/Power` },
    };
  }
  if (url === `${RF}/Managers/bmc` && stat.bmc) return { "@odata.id": url, Id: "bmc", Name: "OpenBmc Manager", PowerState: "On", Status: OK(), ...stat.bmc };
  if (url === `${RF}/Managers/bmc/EthernetInterfaces/eth0` && live.eth0) return { "@odata.id": url, Id: "eth0", ...live.eth0 };
  return undefined;
}
