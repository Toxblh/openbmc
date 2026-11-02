// Панель 1Э8СВ-uATX, ТВГИ.469555.445. Положения: рисунки 1 и 2 руководства
// ТВГИ.469555.445РЭ, по концам выносных линий; f1/f2: пиксель рисунка при 330 dpi
// (плата 1062 пикселя = 244 мм) -> мм от левого верхнего угла. approx: на
// чертеже такого нет, место выбрано по соседним деталям.
const MM = 244 / 1062;
const f1 = (x, y) => [(x - 190) * MM, (y - 152) * MM];
const f2 = (x, y) => [(x - 190) * MM, (y - 138) * MM];
const r1 = (x1, y1, x2, y2) => [...f1(x1, y1), (x2 - x1) * MM, (y2 - y1) * MM];
const r2 = (x1, y1, x2, y2) => [...f2(x1, y1), (x2 - x1) * MM, (y2 - y1) * MM];

const fan = (id, r, chan, src, name) => ({ id, type: "fan", rect: r, z: 3, title: `Вентилятор ${id}`, chan, src, name });
const sata = (id, r, ch) => ({ id, type: "sata", rect: r, z: 4, title: `SATA, канал ${ch}`, sata: ch });
const dimm = (id, r, ch) => ({ id, type: "dimm", rect: r, z: 11, title: `DDR4, канал ${ch}` });
const pcie = (id, r, lanes) => ({ id, type: "pcie", rect: r, z: 4, title: `PCI Express x16, подключено линий: ${lanes}` });

export default {
  name: "1Э8СВ-uATX",
  doc: "ТВГИ.469555.445РЭ",
  size: [244, 244],
  holes: [f1(335, 180), f1(910, 176), f1(1225, 170), f1(233, 865), f1(910, 862), f1(1228, 862), f1(233, 1065), f1(910, 1150), f1(1222, 1185)],
  parts: [
    { id: "CPU", type: "chip", rect: r1(635, 405, 890, 595), z: 3, title: "Процессор Эльбрус-8СВ" },
    { id: "КПИ-2", type: "chip", rect: r1(877, 932, 1035, 1090), z: 2.5, title: "КПИ-2 (1991ВГ2Я), южный мост" },
    { id: "BMC", type: "chip", rect: r1(295, 445, 378, 522), z: 1.5, approx: true, title: "BMC AST2500: крупная микросхема рядом с VGA, в руководстве её места нет" },
    dimm("X46", r1(400, 218, 1040, 238), 3),
    dimm("X44", r1(400, 255, 1040, 275), 2),
    dimm("X16", r1(400, 722, 1040, 742), 0),
    dimm("X45", r1(400, 760, 1040, 780), 1),
    pcie("X31", r1(378, 842, 763, 870), 8),
    pcie("X30", r1(378, 932, 763, 962), 8),
    pcie("X32", r1(378, 1108, 763, 1140), 2),
    { id: "X27", type: "power", rect: r1(665, 158, 888, 202), z: 7, title: "Питание ATX, 24 контакта" },
    { id: "X29", type: "power", rect: r1(573, 158, 652, 202), z: 7, title: "Питание ATX, 8 контактов" },
    { id: "X42", type: "battery", rect: r2(570, 992, 680, 1090), z: 2, title: "Батарея CR2032" },
    { id: "X48", type: "m2", rect: r2(1102, 482, 1197, 520), z: 2, title: "M.2 2280 SATA, канал 3", sata: 3 },
    sata("X26", r2(1197, 735, 1222, 802), 0), sata("X20", r2(1100, 575, 1125, 640), 1), sata("X21", r2(1150, 655, 1175, 722), 2),
    sata("X22", r2(1197, 655, 1222, 722), 4), sata("X23", r2(1148, 735, 1172, 802), 5), sata("X24", r2(1195, 575, 1220, 640), 6),
    sata("X25", r2(1148, 575, 1172, 640), 7),
    { id: "X4", type: "rear", port: "bmc", usb: true, rect: r1(185, 235, 300, 305), z: 15, title: "BMC LAN и USB 2.0 (порты 4 и 6)" },
    { id: "X28", type: "rear", rect: r1(160, 470, 255, 600), z: 10, title: "VGA" },
    { id: "X15", type: "rear", port: "host", rect: r1(185, 617, 278, 685), z: 13, title: "LAN хоста, интерфейс 1" },
    { id: "X14", type: "rear", port: "host", rect: r1(185, 700, 278, 768), z: 13, title: "LAN хоста, интерфейс 0" },
    { id: "X19", type: "header", usb: true, rect: r1(320, 1172, 388, 1200), z: 2, title: "USB 2.0 (порты 1 и 3), вывод на переднюю панель" },
    fan("X37", r1(1037, 338, 1082, 360), "LM96163 процессора", "6-004c", "вентилятор"),
    fan("X38", r1(952, 802, 998, 820), "LM96163 КПИ-2", "2-004c", "вентилятор"),
    { id: "X39", type: "fan", rect: r1(1003, 802, 1048, 820), z: 3, title: "Вентилятор X39, без тахометра", chan: "LM96163 КПИ-2", noTach: "2-004c" },
    { id: "X40", type: "fan", rect: r1(900, 802, 945, 820), z: 3, title: "Вентилятор X40, без тахометра", chan: "LM96163 КПИ-2", noTach: "2-004c" },
    fan("X5", r1(463, 155, 488, 200), "EMC2305 0, канал 0", "2-002e", "вентилятор 0"),
    fan("X6", r1(498, 155, 525, 200), "EMC2305 0, канал 1", "2-002e", "вентилятор 1"),
    fan("X41", r1(535, 155, 560, 200), "EMC2305 0, канал 2", "2-002e", "вентилятор 2"),
    fan("X54", r2(1208, 246, 1250, 262), "EMC2305 0, канал 3", "2-002e", "вентилятор 3"),
    fan("X55", r2(1208, 283, 1250, 300), "EMC2305 0, канал 4", "2-002e", "вентилятор 4"),
    fan("X56", r2(1208, 318, 1250, 335), "EMC2305 1, канал 0", "2-002c", "вентилятор 5"),
    fan("X57", r2(1208, 425, 1250, 442), "EMC2305 1, канал 1", "2-002c", "вентилятор 6"),
    fan("X58", r2(1208, 460, 1250, 477), "EMC2305 1, канал 2", "2-002c", "вентилятор 7"),
    fan("X59", r2(1222, 958, 1240, 1000), "EMC2305 1, канал 3", "2-002c", "вентилятор 8"),
    fan("X60", r2(1222, 1040, 1240, 1082), "EMC2305 1, канал 4", "2-002c", "вентилятор 9"),
  ],
  // what the board's sensors say; heat: a halo around a real sensor, r in mm
  temps: [
    { id: "cpu", at: f1(762, 500), z: 8, hz: 3.2, r: 34, title: "Процессор, кристалл", srcs: /^5-00/, note: "Показан максимум. Где эти точки лежат на кристалле, данными не подтверждено, поэтому отдельно они не рисуются." },
    { id: "iohub", at: f1(956, 1010), z: 7, hz: 2.7, r: 24, title: "КПИ-2", src: "2-004c", name: "IOHUB Temp" },
    { id: "lm-cpu", at: f1(1060, 395), z: 5, r: 9, approx: true, title: "LM96163 процессора", src: "6-004c", name: "Internal Temp" },
    { id: "lm-iohub", at: f1(975, 842), z: 5, r: 9, approx: true, title: "LM96163 КПИ-2", src: "2-004c", name: "Internal Temp" },
    { id: "vrm-uncore", at: f1(470, 560), z: 6, r: 13, approx: true, title: "Источник +UNCORE", src: "6-0044", name: "температура" },
    { id: "vrm-pc", at: f1(470, 410), z: 6, r: 13, approx: true, title: "Power Control", src: "6-0060", name: "температура" },
    { id: "vrm-01", at: f1(1040, 680), z: 6, r: 13, approx: true, title: "Источник +1V2_01", src: "6-0045", name: "температура" },
    { id: "vrm-23", at: f1(1030, 300), z: 6, r: 13, approx: true, title: "Источник +1V2_23", src: "6-0046", name: "температура" },
  ],
  // Рисунок 9: GE0 -> X14, GE1 -> X15. PCI-функция -> GE: по порядку, не проверено
  nics: { "0000:01:01.0": "X14", "0000:01:01.1": "X15", enp1s1f0: "X14", enp1s1f1: "X15" },
  nicNote: "Какой PCI-функции какой разъём соответствует, выведено по нумерации (GE0 → X14, GE1 → X15), кабелем не проверено.",
  // AHCI numbers its ports over both controllers: ataN = SATA N-1 (рисунок 9); checked on one: M.2 at ata4
  ata: (n) => n - 1,
  // Рисунок 9: X19 USB01, USB03; X4 USB10, USB12; USB13 is the BMC's. EHCI 01:0a: USB0x, 01:0b: USB1x
  // (checked: the BMC's virtual hub is 4-4). Root hub -> controller, port N -> USBx(N-1)
  usb: { roots: { 1: 0, 2: 0, 3: 1, 4: 1 }, ports: { "01": "X19", "03": "X19", 10: "X4", 12: "X4", 13: "BMC" } },
  // X30, X31 are the two ports of up to x16, X32 the x4 ports with nothing fixed on them (01:05.0 is the BMC).
  // Which port is which slot isn't in the manual: rootSlot stays empty until someone has looked at the board.
  pcie: {
    groups: [
      { slots: ["X31", "X30"], roots: ["0000:01:08.0", "0000:01:09.0"] },
      { slots: ["X32"], roots: ["0000:01:04.0", "0000:01:06.0", "0000:01:07.0"], inferred: true },
    ],
    rootSlot: {},
  },
};
