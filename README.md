# postmarketOS mainline-port — Samsung Galaxy Tab A 10.5 (2018, Wi-Fi)

Mainline-portning av **gta2xlwifi** (Samsung Galaxy Tab A 10.5 2018, Wi-Fi),
SoC **Qualcomm SDM450 / MSM8953**, till postmarketOS på mainline-kärnan.

Bygger ovanpå pmOS generiska device-profil **`qcom-msm8953`** (systemd-edge,
XFCE), kärna `linux-postmarketos-qcom-msm8953` (msm8953-mainline/linux v7.0.9-r0),
bootloader **lk2nd**.

## Status

| Funktion | Läge |
|---|---|
| Boot (kärna + vår dtb) | ✅ |
| eMMC / lagring | ✅ (`mmcblk1`) |
| Boot till userspace + **ssh** | ✅ (`ssh simon@172.16.42.1`, lösen `1234`) |
| **Display** | ✅ via **simpledrm** på lk2nd:s framebuffer (korrekt färg, boot→login) |
| Touch (ST FTS) | 🟡 driver + i2c-fix på plats, under verifiering |
| Riktig DSI-panel (HX8279) | ⛔ uppskjuten (ISL98608 i2c-config saknas) — simpledrm används istället |
| Modem | medvetet disablat (Wi-Fi-variant) |
| Wi-Fi/BT, sensorer, kamera, ljud | ej påbörjat |

## Struktur

```
mainline-port/
├── msm8953-samsung-gta2xlwifi.dts   # ← KÄLLAN (device tree). Redigera denna.
├── apply-dts.sh                     # kopiera dts → kärnträd + verifiera (out-of-tree)
├── build-bootimg.sh                 # bygg RAM-bootbar boot.img ur kärn-apk:n
├── images/boot.img                  # senaste fungerande boot.img (gitignored)
├── pmaports-overlay/                # ändrade pmaports-filer (referens)
│   ├── config-postmarketos-qcom-msm8953.aarch64
│   ├── APKBUILD
│   └── config-changes.diff          # config-deltat vs upstream
├── downstream-dts/                  # nedströms Android-dts (hårdvarureferens)
├── templates/                       # mainline msm8953-dts som mall (daisy, mido)
└── reference/live-fdt-downstream.dts# dekompilerad /sys/firmware/fdt från plattan
```

Kärnträdet (`~/pmos/mainline-build/linux-7.0.9-r0`) ligger utanför repot.

## Arbetsflöde

```bash
cd mainline-port
# 1. redigera msm8953-samsung-gta2xlwifi.dts
./apply-dts.sh                    # kopiera in i kärnträdet + verifiera att den kompilerar
                                  # (VIKTIGT: kör alltid denna före build — annars byggs gammal dts)

# 2. dts-bara ändring → räcker att kompilera om dtb + boot.img:
./build-bootimg.sh                # → images/boot.img

# 2b. config-/kärnändring → bygg om kärnan först:
pmbootstrap build --src=$HOME/pmos/mainline-build/linux-7.0.9-r0 \
    linux-postmarketos-qcom-msm8953
./build-bootimg.sh

# 3. RAM-boota (ofarligt, inget flashas):
#    plattan i lk2nd-fastboot, sen:
fastboot boot images/boot.img
```

## Hårt förvärvade läxor (läs innan du ändrar)

- **PMIC:** gta2 har **bara pm8953** — ingen pmi8950/pmi8937. Inkludera INTE
  `pmi8950.dtsi` (SPMI-probet failar -EIO och blockerar allt bakom).
- **dtb-val:** lk2nd väljer dtb via `qcom,msm-id = <0x152 0x0>` + `qcom,board-id =
  <0x08 0x04>` (lästa ur plattans live-fdt). Utan dem laddas downstream-dtb:n.
- **Display:** kärnan saknar färdig HX8279-panelväg som tänder skärmen (ISL98608
  behöver i2c-config, ingen mainline-driver). Lösning: **simpledrm** på lk2nd:s
  cont_splash-framebuffer (`0x90001000`), msm-DSI **disablad**, panel-rail:erna
  always-on. Framebuffer-format som ger rätt bild: **1200×1920, stride 1200×3,
  `r8g8b8` (24bpp)** — 32bpp gav skev/grumlig bild.
- **Touch:** i2c_1 (`78b5000`) använder gpio **2–3**. `gpio-reserved-ranges` får
  därför INTE reservera dem → använd `<0 2>` (inte daisys `<0 4>`). Touch-drivern
  är `CONFIG_TOUCHSCREEN_STMFTS`.
- **Root:** rätt UUID:er (ur installen på `mmcblk1p47`): root
  `07d2abef-9505-4fc9-a6da-b6e69594ae30`, boot `1298aa1d-1977-4172-a50b-d7aead5d4569`.
- **Bygg-hygien:** kör ALDRIG `make` (defconfig/olddefconfig/dtb) på Arch-värden
  mot kärnträdet — host-gcc/glibc förorenar configen (`CC_IS_GCC`, `fixdep`) och
  bryter Alpine/musl-chroot-bygget. `apply-dts.sh`/`build-bootimg.sh` bygger
  out-of-tree; config-ändringar görs för hand (chrootens syncconfig löser deps).

## Kvar att göra

1. Verifiera touch (STMFTS på i2c_1 @ 0x49).
2. Riktig HX8279-DSI-panel: ISL98608 i2c-VSP/VSN-config + verifiera genererad
   init-sekvens, sen byt från simpledrm tillbaka till `&mdss`/`&mdss_dsi0`.
3. Wi-Fi/BT (`&wcnss`), sensorer, ljud, kamera, laddare/fuel-gauge.
4. Städa `drm_kms_helper duplicate symbol` (kosmetiskt).
