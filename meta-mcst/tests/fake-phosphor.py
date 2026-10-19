#!/usr/bin/env python3
"""Test stand-in for the phosphor D-Bus services of a REIMU BMC.

Publishes, on the system bus, objects shaped like the ones phosphor-hwmon,
phosphor-inventory-manager, phosphor-state-manager (REIMU's reimupwr),
phosphor-logging and the software manager create on real hardware, plus a
generic xyz.openbmc_project.ObjectMapper over them (and over real services
listed in REAL_SERVICES, e.g. reimu-pnsd). This lets the *real* bmcweb
Redfish code run in a VM without the ASPEED hardware.

It is a test fixture only; it is not part of the firmware.
"""
import hashlib
import os
import random
import re
import shutil
import tarfile
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

D = dbus.Double
S = dbus.String
B = dbus.Boolean
OBJ = {}  # path -> {iface: {prop: typed value}}

INV = "/xyz/openbmc_project/inventory/system"
CHASSIS = INV + "/chassis"
BOARD = CHASSIS + "/motherboard"
UNIT = "xyz.openbmc_project.Sensor.Value.Unit."


def add(path, ifaces):
    OBJ.setdefault(path, {}).update(ifaces)


def asset(mfr, model, sn, pn):
    return {"Manufacturer": S(mfr), "Model": S(model), "SerialNumber": S(sn), "PartNumber": S(pn),
            "BuildDate": S(""), "SparePartNumber": S("")}


# ---------------- inventory ----------------
add(INV, {"xyz.openbmc_project.Inventory.Item.System": {},
          "xyz.openbmc_project.Inventory.Item": {"PrettyName": S("E8C-mATX"), "Present": B(True)},
          "xyz.openbmc_project.Inventory.Decorator.Asset": asset("MCST", "E8C-mATX (test)", "TEST-0001", "TVGI.00306-01")})
add(CHASSIS, {"xyz.openbmc_project.Inventory.Item.Chassis": {"Type": S("xyz.openbmc_project.Inventory.Item.Chassis.ChassisType.RackMount")},
              "xyz.openbmc_project.Inventory.Item": {"PrettyName": S("Шасси"), "Present": B(True)},
              "xyz.openbmc_project.Inventory.Decorator.Asset": asset("MCST", "Server 1U (test)", "CH-0001", "643.DACN.20001-01"),
              "xyz.openbmc_project.Association.Definitions": {"Associations": dbus.Array([], signature="(sss)")}})
for i in range(2):
    add(f"{INV}/chassis/motherboard/cpu{i}", {
        "xyz.openbmc_project.Inventory.Item.Cpu": {"Family": S("Elbrus"), "CoreCount": dbus.UInt16(8), "ThreadCount": dbus.UInt16(8)},
        "xyz.openbmc_project.Inventory.Item": {"PrettyName": S(f"CPU{i}"), "Present": B(True)},
        "xyz.openbmc_project.Inventory.Decorator.Asset": asset("MCST", "Elbrus-8C", f"CPU-{i}", ""),
        "xyz.openbmc_project.State.Decorator.OperationalStatus": {"Functional": B(True)}})
for i in range(4):
    add(f"{INV}/chassis/motherboard/dimm{i}", {
        "xyz.openbmc_project.Inventory.Item.Dimm": {"MemorySizeInKB": dbus.UInt32(16 * 1024 * 1024), "MemoryType": S("xyz.openbmc_project.Inventory.Item.Dimm.DeviceType.DDR3")},
        "xyz.openbmc_project.Inventory.Item": {"PrettyName": S(f"DIMM{i}"), "Present": B(True)},
        "xyz.openbmc_project.Inventory.Decorator.Asset": asset("Kingston", "KVR16R11D4", f"DIMM-{i}", ""),
        "xyz.openbmc_project.State.Decorator.OperationalStatus": {"Functional": B(True)}})

# ---------------- state (REIMU reimupwr) ----------------
HOST = "/xyz/openbmc_project/state/host0"
add(HOST, {"xyz.openbmc_project.State.Host": {
    "CurrentHostState": S("xyz.openbmc_project.State.Host.HostState.Off"),
    "RequestedHostTransition": S("xyz.openbmc_project.State.Host.Transition.Off")}})
add("/xyz/openbmc_project/state/chassis0", {"xyz.openbmc_project.State.Chassis": {
    "CurrentPowerState": S("xyz.openbmc_project.State.Chassis.PowerState.Off"),
    "RequestedPowerTransition": S("xyz.openbmc_project.State.Chassis.Transition.Off")}})
add("/xyz/openbmc_project/state/bmc0", {"xyz.openbmc_project.State.BMC": {
    "CurrentBMCState": S("xyz.openbmc_project.State.BMC.BMCState.Ready"),
    "RequestedBMCTransition": S("xyz.openbmc_project.State.BMC.Transition.None")}})

# ---------------- power cap (phosphor-settings) ----------------
add("/xyz/openbmc_project/control/host0/power_cap", {"xyz.openbmc_project.Control.Power.Cap": {
    "PowerCap": dbus.UInt32(0), "PowerCapEnable": B(False)}})

# ---------------- users (phosphor-user-manager) ----------------
add("/xyz/openbmc_project/user/root", {"xyz.openbmc_project.User.Attributes": {
    "UserGroups": dbus.Array(["redfish", "ipmi", "ssh", "web"], signature="s"), "UserPrivilege": S("priv-admin"),
    "UserEnabled": B(True), "UserLockedForFailedAttempt": B(False)}})

# ---------------- BMC firmware ----------------
SW = "/xyz/openbmc_project/software/reimu"
add(SW, {"xyz.openbmc_project.Software.Version": {"Version": S("reimu-1.0.2-web (test VM)"), "Purpose": S("xyz.openbmc_project.Software.Version.VersionPurpose.BMC")},
         "xyz.openbmc_project.Software.Activation": {"Activation": S("xyz.openbmc_project.Software.Activation.Activations.Active"),
                                                      "RequestedActivation": S("xyz.openbmc_project.Software.Activation.RequestedActivations.None")}})
add("/xyz/openbmc_project/software/functional", {"xyz.openbmc_project.Association": {"endpoints": dbus.Array([SW], signature="s")}})

# ---------------- event log ----------------
for i, (sev, msg) in enumerate([("Informational", "BMC загрузился"), ("Warning", "CPU0: температура выше предупредительного порога"),
                                ("Critical", "Вентилятор FAN2 остановлен")], start=1):
    add(f"/xyz/openbmc_project/logging/entry/{i}", {"xyz.openbmc_project.Logging.Entry": {
        "Id": dbus.UInt32(i), "Severity": S(f"xyz.openbmc_project.Logging.Entry.Level.{sev}"), "Message": S(msg),
        "Timestamp": dbus.UInt64(int(time.time() * 1000) - (4 - i) * 3600000), "Resolved": B(False),
        "AdditionalData": dbus.Array([], signature="s")}})

# ---------------- sensors (phosphor-hwmon style) ----------------
SENSORS = []  # (path, base, noise, present_only_when_on)


def sensor(kind, name, unit, value, warn=None, crit=None, platform=True):
    path = f"/xyz/openbmc_project/sensors/{kind}/{name}"
    ifs = {"xyz.openbmc_project.Sensor.Value": {"Value": D(value), "Unit": S(UNIT + unit)},
           "xyz.openbmc_project.State.Decorator.OperationalStatus": {"Functional": B(True)},
           "xyz.openbmc_project.State.Decorator.Availability": {"Available": B(True)},
           "xyz.openbmc_project.Association.Definitions": {"Associations": dbus.Array([("chassis", "all_sensors", CHASSIS)], signature="(sss)")}}
    if warn:
        ifs["xyz.openbmc_project.Sensor.Threshold.Warning"] = {"WarningLow": D(warn[0]), "WarningHigh": D(warn[1]),
                                                                "WarningAlarmLow": B(False), "WarningAlarmHigh": B(False)}
    if crit:
        ifs["xyz.openbmc_project.Sensor.Threshold.Critical"] = {"CriticalLow": D(crit[0]), "CriticalHigh": D(crit[1]),
                                                                 "CriticalAlarmLow": B(False), "CriticalAlarmHigh": B(False)}
    SENSORS.append((path, value, ifs, platform))
    if not platform:  # BMC-own sensors exist always
        add(path, ifs)


sensor("temperature", "BMC_Temp", "DegreesC", 41.0, (0, 70), (-5, 85), platform=False)
sensor("voltage", "P3V3_STBY", "Volts", 3.31, (3.1, 3.5), (3.0, 3.6), platform=False)
sensor("voltage", "P5V_STBY", "Volts", 5.02, (4.75, 5.25), (4.5, 5.5), platform=False)
# platform sensors appear when the host is powered (hwmon-instantiator on REIMU)
sensor("temperature", "CPU0_Temp", "DegreesC", 52.0, (0, 75), (-5, 90))
sensor("temperature", "CPU1_Temp", "DegreesC", 49.5, (0, 75), (-5, 90))
sensor("temperature", "Board_Temp", "DegreesC", 34.0, (0, 60), (-5, 70))
sensor("fan_tach", "FAN1", "RPMS", 5400, (1000, 12000), (500, 15000))
sensor("fan_tach", "FAN2", "RPMS", 5200, (1000, 12000), (500, 15000))
sensor("voltage", "P12V", "Volts", 12.05, (11.4, 12.6), (10.8, 13.2))
sensor("voltage", "VCORE0", "Volts", 0.98, (0.9, 1.1), (0.85, 1.15))


def all_sensor_paths():
    return [p for p in OBJ if p.startswith("/xyz/openbmc_project/sensors/")]


def update_assoc():
    OBJ.setdefault(CHASSIS + "/all_sensors", {})["xyz.openbmc_project.Association"] = {"endpoints": dbus.Array(all_sensor_paths(), signature="s")}
    OBJ.setdefault(CHASSIS + "/sensors", {})["xyz.openbmc_project.Association"] = {"endpoints": dbus.Array(all_sensor_paths(), signature="s")}


update_assoc()

# mapper data for real services running in the VM
REAL_SERVICES = {"/xyz/openbmc_project/reimu/pns": {"xyz.openbmc_project.Reimu.Pns": ["xyz.openbmc_project.Reimu.Pns", "org.freedesktop.DBus.Properties"]},
                 "/xyz/openbmc_project/reimu/firmware": {"xyz.openbmc_project.Reimu.Firmware": ["xyz.openbmc_project.Reimu.Firmware", "org.freedesktop.DBus.Properties"]}}

NAMES = ["xyz.openbmc_project.State.Host", "xyz.openbmc_project.State.Chassis", "xyz.openbmc_project.State.BMC",
         "xyz.openbmc_project.Inventory.Manager", "xyz.openbmc_project.Hwmon-1.Hwmon1", "xyz.openbmc_project.Software.BMC.Updater",
         "xyz.openbmc_project.Logging", "xyz.openbmc_project.ObjectMapper", "xyz.openbmc_project.User.Manager", "xyz.openbmc_project.Settings"]
SERVICE = "xyz.openbmc_project.Inventory.Manager"  # every fake object is reachable under any owned name


def service_of(path):
    if path.startswith("/xyz/openbmc_project/sensors"):
        return "xyz.openbmc_project.Hwmon-1.Hwmon1"
    if path.startswith("/xyz/openbmc_project/state/host"):
        return "xyz.openbmc_project.State.Host"
    if path.startswith("/xyz/openbmc_project/state/chassis"):
        return "xyz.openbmc_project.State.Chassis"
    if path.startswith("/xyz/openbmc_project/state/bmc"):
        return "xyz.openbmc_project.State.BMC"
    if path.startswith("/xyz/openbmc_project/software"):
        return "xyz.openbmc_project.Software.BMC.Updater"
    if path.startswith("/xyz/openbmc_project/logging"):
        return "xyz.openbmc_project.Logging"
    if path.startswith("/xyz/openbmc_project/control"):
        return "xyz.openbmc_project.Settings"
    if path.startswith("/xyz/openbmc_project/user"):
        return "xyz.openbmc_project.User.Manager"
    if path.endswith("/all_sensors") or path.endswith("/sensors") or path.endswith("/functional"):
        return "xyz.openbmc_project.ObjectMapper"
    return SERVICE


def ifaces_of(path):
    std = ["org.freedesktop.DBus.Properties", "org.freedesktop.DBus.Introspectable"]
    return list(OBJ[path].keys()) + std


class Fake(dbus.service.FallbackObject):
    # ---- properties ----
    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v", rel_path_keyword="path")
    def Get(self, iface, prop, path):
        try:
            return OBJ[path][iface][prop]
        except KeyError:
            raise dbus.exceptions.DBusException(f"no property {iface}.{prop} at {path}", name="org.freedesktop.DBus.Error.UnknownProperty")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}", rel_path_keyword="path")
    def GetAll(self, iface, path):
        if path not in OBJ:
            raise dbus.exceptions.DBusException(f"no object {path}", name="org.freedesktop.DBus.Error.UnknownObject")
        return dbus.Dictionary(OBJ[path].get(iface, {}), signature="sv")

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ssv", out_signature="", rel_path_keyword="path")
    def Set(self, iface, prop, value, path):
        cur = OBJ.get(path, {}).get(iface, {})
        if prop not in cur:
            raise dbus.exceptions.DBusException("unknown property", name="org.freedesktop.DBus.Error.UnknownProperty")
        cur[prop] = type(cur[prop])(value)
        self.PropertiesChanged(iface, {prop: cur[prop]}, [], path=path[1:])  # fallback at "/": relative path
        if path.startswith("/xyz/openbmc_project/software/") and prop == "RequestedActivation" and str(value).endswith(".Active"):
            GLib.timeout_add(300, activate_image, path)
        if path == HOST and prop == "RequestedHostTransition":
            GLib.timeout_add(1500, host_transition, str(value))

    @dbus.service.signal("org.freedesktop.DBus.Properties", signature="sa{sv}as", rel_path_keyword="path")
    def PropertiesChanged(self, iface, changed, invalidated, path=None):
        pass

    @dbus.service.signal("org.freedesktop.DBus.ObjectManager", signature="oa{sa{sv}}", rel_path_keyword="path")
    def InterfacesAdded(self, obj, ifaces, path=None):
        pass

    # ---- object manager (at "/") ----
    @dbus.service.method("org.freedesktop.DBus.ObjectManager", in_signature="", out_signature="a{oa{sa{sv}}}", rel_path_keyword="path")
    def GetManagedObjects(self, path):
        out = {}
        for p, ifs in OBJ.items():
            if p.startswith(path.rstrip("/") + "/") or path == "/":
                out[dbus.ObjectPath(p)] = dbus.Dictionary({i: dbus.Dictionary(v, signature="sv") for i, v in ifs.items()}, signature="sa{sv}")
        return dbus.Dictionary(out, signature="oa{sa{sv}}")

    # ---- introspection ----
    @dbus.service.method("org.freedesktop.DBus.Introspectable", in_signature="", out_signature="s", rel_path_keyword="path")
    def Introspect(self, path):
        children = set()
        pre = path.rstrip("/") + "/"
        for p in OBJ:
            if p.startswith(pre):
                children.add(p[len(pre):].split("/")[0])
        x = ['<node>']
        for i in OBJ.get(path, {}):
            x.append(f'<interface name="{i}">')
            for prop, val in OBJ[path][i].items():
                x.append(f'<property name="{prop}" type="{val.signature if hasattr(val, "signature") and val.signature else sig(val)}" access="readwrite"/>')
            x.append('</interface>')
        for c in sorted(children):
            x.append(f'<node name="{c}"/>')
        x.append('</node>')
        return "\n".join(x)

    # ---- user manager ----
    @dbus.service.method("xyz.openbmc_project.User.Manager", in_signature="s", out_signature="a{sv}", rel_path_keyword="path")
    def GetUserInfo(self, user, path):
        return dbus.Dictionary({"UserPrivilege": S("priv-admin"), "UserGroups": dbus.Array(["redfish", "ipmi", "ssh", "web"], signature="s"),
                                "UserEnabled": B(True), "UserLockedForFailedAttempt": B(False), "RemoteUser": B(False),
                                "UserPasswordExpired": B(False)}, signature="sv")

    # ---- object mapper ----
    @dbus.service.method("xyz.openbmc_project.ObjectMapper", in_signature="sas", out_signature="a{sas}", rel_path_keyword="path")
    def GetObject(self, obj, ifaces, path):
        if obj in REAL_SERVICES:
            return REAL_SERVICES[obj]
        if obj not in OBJ or (ifaces and not set(ifaces) & set(OBJ[obj])):
            raise dbus.exceptions.DBusException(f"{obj} not found", name="xyz.openbmc_project.Common.Error.ResourceNotFound")
        return {service_of(obj): ifaces_of(obj)}

    def _subtree(self, root, depth, ifaces):
        root = root.rstrip("/") or "/"
        out = {}
        items = list(OBJ.items()) + [(p, {i: {} for i in v[next(iter(v))]}) for p, v in REAL_SERVICES.items()]
        for p, ifs in items:
            if root != "/" and not p.startswith(root + "/"):
                continue
            rel = p[len(root):].strip("/")
            if depth > 0 and rel.count("/") + 1 > depth:
                continue
            if ifaces and not set(ifaces) & set(ifs):
                continue
            if p in REAL_SERVICES:
                out[p] = REAL_SERVICES[p]
            else:
                out[p] = {service_of(p): ifaces_of(p)}
        return out

    @dbus.service.method("xyz.openbmc_project.ObjectMapper", in_signature="sias", out_signature="a{sa{sas}}", rel_path_keyword="path")
    def GetSubTree(self, root, depth, ifaces, path):
        return dbus.Dictionary(self._subtree(root, depth, ifaces), signature="sa{sas}")

    @dbus.service.method("xyz.openbmc_project.ObjectMapper", in_signature="sias", out_signature="as", rel_path_keyword="path")
    def GetSubTreePaths(self, root, depth, ifaces, path):
        return dbus.Array(sorted(self._subtree(root, depth, ifaces)), signature="s")

    @dbus.service.method("xyz.openbmc_project.ObjectMapper", in_signature="sas", out_signature="a{sa{sas}}", rel_path_keyword="path")
    def GetAncestors(self, obj, ifaces, path):
        out = {}
        parts = obj.strip("/").split("/")
        for n in range(1, len(parts)):
            p = "/" + "/".join(parts[:n])
            if p in OBJ and (not ifaces or set(ifaces) & set(OBJ[p])):
                out[p] = {service_of(p): ifaces_of(p)}
        return dbus.Dictionary(out, signature="sa{sas}")


def sig(v):
    return {dbus.Double: "d", dbus.String: "s", dbus.Boolean: "b", dbus.UInt16: "q", dbus.UInt32: "u", dbus.UInt64: "t"}.get(type(v), "v")


fake = None


def host_transition(req):
    on = req.endswith(".On") or req.endswith(".Reboot") or req.endswith("GracefulWarmReboot") or req.endswith("ForceWarmReboot")
    state = "Running" if on else "Off"
    OBJ[HOST]["xyz.openbmc_project.State.Host"]["CurrentHostState"] = S(f"xyz.openbmc_project.State.Host.HostState.{state}")
    OBJ["/xyz/openbmc_project/state/chassis0"]["xyz.openbmc_project.State.Chassis"]["CurrentPowerState"] = S(
        f"xyz.openbmc_project.State.Chassis.PowerState.{'On' if on else 'Off'}")
    fake.PropertiesChanged("xyz.openbmc_project.State.Host", {"CurrentHostState": OBJ[HOST]["xyz.openbmc_project.State.Host"]["CurrentHostState"]}, [], path=HOST[1:])
    # platform sensors are instantiated on power on and removed on power off
    for p, base, ifs, platform in SENSORS:
        if platform:
            if on:
                add(p, ifs)
            else:
                OBJ.pop(p, None)
    update_assoc()
    return False


# ---- static layout software updater (phosphor-image-updater) ----
# bmcweb's UpdateService stores an uploaded tar in /tmp/images; the updater
# unpacks it, publishes a Ready software object, and on activation copies the
# image-* files to /run/initramfs, where the shutdown script flashes them.
IMAGES, STAGE = "/tmp/images", "/run/initramfs"


def scan_images():
    os.makedirs(IMAGES, exist_ok=True)
    for f in os.listdir(IMAGES):
        p = os.path.join(IMAGES, f)
        if not os.path.isfile(p):
            continue
        try:
            with tarfile.open(p) as t:
                manifest = t.extractfile("MANIFEST").read().decode()
                version = re.search(r"^version=(.*)$", manifest, re.M).group(1)
                vid = hashlib.sha512(version.encode()).hexdigest()[:8]
                t.extractall(os.path.join(IMAGES, vid))
        except Exception as e:  # not an image: the real updater logs and drops it
            print("bad image", f, e, flush=True)
            os.unlink(p)
            continue
        os.unlink(p)
        path = f"/xyz/openbmc_project/software/{vid}"
        ifs = {"xyz.openbmc_project.Software.Version": {"Version": S(version), "Purpose": S("xyz.openbmc_project.Software.Version.VersionPurpose.BMC")},
               "xyz.openbmc_project.Software.Activation": {"Activation": S("xyz.openbmc_project.Software.Activation.Activations.Ready"),
                                                           "RequestedActivation": S("xyz.openbmc_project.Software.Activation.RequestedActivations.None")}}
        add(path, ifs)
        fake.InterfacesAdded(dbus.ObjectPath(path), dbus.Dictionary({i: dbus.Dictionary(v, signature="sv") for i, v in ifs.items()}, signature="sa{sv}"),
                             path="xyz/openbmc_project/software")
        print("image", version, "ready as", path, flush=True)
    return True


def activate_image(path):
    vid = path.rsplit("/", 1)[1]
    os.makedirs(STAGE, exist_ok=True)
    src = os.path.join(IMAGES, vid)
    for f in os.listdir(src):
        if f.startswith("image-"):
            shutil.copyfile(os.path.join(src, f), os.path.join(STAGE, f))
    act = OBJ[path]["xyz.openbmc_project.Software.Activation"]
    act["Activation"] = S("xyz.openbmc_project.Software.Activation.Activations.Active")
    fake.PropertiesChanged("xyz.openbmc_project.Software.Activation", {"Activation": act["Activation"]}, [], path=path[1:])
    print("image", vid, "staged in", STAGE, flush=True)
    return False


def jitter():
    for p, base, ifs, platform in SENSORS:
        if p in OBJ:
            v = base * (1 + random.uniform(-0.02, 0.02))
            OBJ[p]["xyz.openbmc_project.Sensor.Value"]["Value"] = D(round(v, 3))
    return True


def main():
    global fake
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    names = [dbus.service.BusName(n, bus) for n in NAMES]  # noqa: F841 (keep names owned)
    fake = Fake(bus, "/")
    GLib.timeout_add(2000, jitter)
    GLib.timeout_add(1000, scan_images)
    print("fake phosphor services ready", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
