# E850-96: 벤더 5.10 커널로 Linux 부팅하기 (A안) — 진행 매뉴얼

기록 시작: 2026-10-31 (KST)  
목표: Android 10 LK(부트로더)는 그대로 두고, Linaro 벤더 5.10 커널 + busybox initramfs → Debian rootfs로 부팅한다. 그 위에서 OV5647 카메라 드라이버를 개발한다.  
선행 문서: [진행 계획](E85096-OV5647-VENDOR510-DEBIAN-PLAN.md), [Chip ID·백업 원시 기록](E85096-OV5647-CHIPID-RAW-LOG.md)

> 이 문서는 진행하면서 계속 덧붙인다. 각 단계에 **성공/실패와 실제 로그**를 함께 적는다. 성공하지 않은 단계는 성공했다고 쓰지 않는다.

---

## 0. 전체 구조

```text
LK (Android 10 부트로더, eMMC boot0 — 건드리지 않음)
  └─ boot 파티션의 boot.img (header v2) 를 읽음
       ├─ kernel : 벤더 5.10 Image (GKI 형태, 대부분의 SoC 드라이버가 모듈)
       ├─ ramdisk: busybox initramfs  ← Android vendor_boot 역할: 모듈 139개 중 필요한 것을 순서대로 적재
       └─ dtb    : s5e3830.dtb + s5e3830-erd3830.dtbo 를 미리 합친 하나의 DTB
                    └─ /chosen bootargs 를 Linux용으로 바꿈
  └─ initramfs /init → root=/dev/mmcblk0p24(userdata)에 넣은 Debian 으로 switch_root
                       root 가 없거나 실패하면 UART(ttySAC0)에 busybox 셸
```

왜 initramfs가 꼭 필요한가: 이 커널 설정에서는 eMMC(`dw_mmc*`), 타이머(`exynos_mct`), 핀 제어(`pinctrl-samsung-core`), 클록(`clk_exynos`), PMIC(`s2mpu12*`)가 전부 **모듈**이다.

```text
CONFIG_MMC_DW=m  CONFIG_MMC_DW_EXYNOS=m  CONFIG_PINCTRL_SAMSUNG=m  CONFIG_EXYNOS_PMU=m  CONFIG_REGULATOR_S2MPU12=m
CONFIG_EXT4_FS=y CONFIG_MMC_BLOCK=y      CONFIG_SERIAL_SAMSUNG=y   CONFIG_BLK_DEV_INITRD=y
```

---

## 1. 준비물과 기존 산출물 확인

| 항목 | 값 |
| --- | --- |
| 커널 빌드 산출물 | Docker 볼륨 `e850-out` (`arch/arm64/boot/Image` 46,926,148 B, 2026-09-27) |
| 커널 릴리스 | `5.10.157-00010-g3b6b9ca51034-dirty` |
| 모듈 | `modules.order` 기준 139개 |
| DTB | `arch/arm64/boot/dts/exynos/s5e3830.dtb`, `board/erd/s5e3830-erd3830.dtbo` |
| 빌드 컨테이너 | `ubuntu:22.04` arm64 (Apple Silicon에서 네이티브 실행) |

```bash
docker run --rm -v e850-out:/o ubuntu:22.04 sh -c 'cat /o/include/config/kernel.release; wc -l < /o/modules.order'
```

### 1.1 5.10 DTB 기본 bootargs의 함정

```bash
docker run --rm -v e850-out:/o ubuntu:22.04 sh -c 'apt-get -qq update >/dev/null; apt-get -qq install -y device-tree-compiler >/dev/null 2>&1; fdtget /o/arch/arm64/boot/dts/exynos/s5e3830.dtb /chosen bootargs'
```

```text
maxcpus=1 console=ram s3c2410_wdt.tmr_atboot=1 clocksource=arch_sys_counter clk_ignore_unused printk.devkmsg=on firmware_class.path=/vendor/firmware ...
```

`s3c2410_wdt.tmr_atboot=1`은 워치독을 부팅하자마자 켠다. Android에서는 `watchdogd`가 주기적으로 신호를 주지만 우리 initramfs에는 없으므로 그대로 두면 일정 시간 뒤 리셋된다. 아래 빌드 스크립트는 bootargs를 새로 쓴다.

---

## 2. boot.img 만들기

도구: [`tools/linux510/`](../tools/linux510/)

| 파일 | 역할 |
| --- | --- |
| `build-bootimg.sh` | 컨테이너 안에서 initramfs·합친 DTB·boot.img 생성 |
| `init` | initramfs의 첫 프로그램(모듈 적재 → root 전환 또는 UART 셸) |
| `autoload.deny` | 부팅 때 자동으로 올리지 않을 모듈(무선, 모뎀, GPU, 카메라, 오디오, 워치독 등). 파일은 initramfs에 들어 있어 셸에서 `modprobe`로 수동 적재 가능 |
| `mkbootimg_v2.py` | Android boot image header v2 패커 |

### 2.1 패커 검증 — 원본과 바이트 단위 일치

LK가 받아들이는 형식인지 확인하려고, 백업해 둔 Android 10 `boot.img`를 풀어서 이 패커로 다시 만들었다.

```text
Android 10 boot.img 헤더: header_version 2, page_size 0x800, kernel_addr 0x10008000, ramdisk_addr 0x10000000,
                          dtb_addr 0x10000000, cmdline "androidboot.hardware=exynos850 androidboot.selinux=permissive buildvariant=eng"
재포장 결과: repack IDENTICAL to stock boot.img (first 24401920 bytes)
```

### 2.2 빌드

```bash
mkdir -p out
docker run --rm -v e850-out:/o:ro -v "$PWD/tools/linux510:/t:ro" -v "$PWD/out:/out" \
    ubuntu:22.04 sh /t/build-bootimg.sh                    # root= 없이: UART 셸까지만
```

실제 출력:

```text
modules: 139 copied, 91 autoload
/out/boot.img: kernel 46926148 ramdisk 8265126 dtb 147218 bytes
```

- 모듈은 `strip --strip-debug`로 디버그 정보를 뺐다(`fimc-is.ko`는 57 MB → initramfs 전체 8 MB).
- boot 파티션 크기는 100 MB(104,857,600 B)이고 boot.img는 55 MB라 들어간다.

---

## 3. 시험 1 — RAM 부팅(`fastboot boot`), eMMC 쓰기 없음

```bash
adb reboot bootloader
fastboot devices                    # ... fastboot
fastboot boot out/boot.img
```

**결과: 실패(멈춤).** `fastboot boot`이 끝나지 않았고 그 뒤 LK fastboot가 `getvar`에도 응답하지 않았다. eMMC는 바뀌지 않았으므로 전원을 껐다 켜면 Android 10으로 돌아온다.

동시에 Mac 쪽 USB-TTL이 실패해 UART 로그를 얻지 못했다.

```text
termios.error: (22, 'Invalid argument')                                         ← 시리얼 포트 설정 실패
kernel: (IOUSBHostFamily) ... device 4 (FT232R USB UART@01110000) endpoint 0x00: status 0xe00002d6 (timeout)
kernel: (IOUSBHostFamily) ... (Exynos3830 LK Bootloader@01140000) endpoint 0x81: aborting 1 requests
```

그때 Chrome(pid 723)이 아직 실행 중이었다. 시험 전에 **Chrome과 ChatGPT 앱을 ⌘Q로 완전히 종료**하고 `pgrep -fl -i 'chrome|chatgpt'`로 확인한다.

LK 바이너리(Linaro dltool의 `lk.bin`) 문자열에서 확인한 사실:

```text
download:            boot            FAILdata too large: %u > %u
FAILdownload data size is bigger than buffer size
Entering fastboot: Abnormal RST_STAT: 0x%x
Entering fastboot: xct boot fail code - %d
```

- `boot` 명령과 크기 초과 오류가 있으므로, 크기 문제였다면 FAIL 응답이 왔어야 한다. 원인은 UART 로그로 확인해야 한다(다음 시험).
- 비정상 리셋 뒤에는 LK가 스스로 fastboot에 들어가는 경로가 있다.

### 다음 시험 전 체크리스트

1. 보드 전원 재투입 → Android 10 부팅 확인(`adb devices`).
2. Chrome·ChatGPT 완전 종료 확인, USB-TTL을 뽑았다 다시 꽂기.
3. UART 기록 시작 → `fastboot getvar max-download-size` 확인.
4. `fastboot boot` 재시도. 크기가 문제면 `Image.gz`(압축 커널) 또는 모듈 수를 줄인 initramfs로 boot.img 크기를 줄인다.

### 3.1 모듈 적재 순서 수정 (시험 1 이후)

커널 빌드 설정(`build.config.erd3830*`)에 Android가 첫 단계에서 쓰는 순서 파일이 있다.

```text
VENDOR_BOOT_MODULE_ORDER=vendor_boot_module_order_s5e3830.cfg
```

```text
exynos-chipid_v2.ko exynos-reboot.ko clk_exynos.ko hardlockup-watchdog.ko exynos_mct.ko
exynos_tty.ko pinctrl-samsung-core.ko i2c-exynos5.ko i2c-s3c2410.ko   ← 이 9개를 먼저, 그다음 modules.order
```

`build-bootimg.sh`가 이 파일(`tools/linux510/vendor_boot_module_order_s5e3830.cfg`)을 앞에 두고 나머지 `modules.order`를 이어 붙이도록 고쳤다. 결과: 자동 적재 92개.

---

## 4. OV5647 CIS 드라이버 (보드 없이 진행한 부분)

묶음: [`modules/ov5647-pablo/`](../modules/ov5647-pablo/README.md)

### 4.1 커널 트리에 적용

```bash
docker run --rm -v e850-kernel:/work/kernel -v "$PWD/modules/ov5647-pablo:/m:ro" e850-build-env:1 \
    sh /m/apply.sh /work/kernel
```

```text
#define SENSOR_NAME_OV5647		219          (두 exynos_is_dt.h에 추가)
16:#include "camera/s5e3830-e850-ov5647-camera.dtsi"   (보드 DT: S5KRC1S → OV5647)
```

### 4.2 모듈 빌드 — 첫 시도 실패와 해결

트리 안에서 단일 대상으로 빌드하면 modpost가 실패한다.

```bash
make O=/work/out ... drivers/media/platform/exynos/camera/sensor/module_framework/cis/is-cis-ov5647.ko
```

```text
ERROR: modpost: "is_dev" [.../is-cis-ov5647.ko] undefined!
ERROR: modpost: "sensor_cis_set_registers" [.../is-cis-ov5647.ko] undefined!
ERROR: modpost: "find_peri_by_cis_id" [.../is-cis-ov5647.ko] undefined!
make[2]: *** Deleting file 'modules-only.symvers'
```

- 이 심볼들은 `fimc-is.ko`가 내보내며 `Module.symvers`(15,280줄)에는 들어 있다. 단일 대상 modpost가 모듈 심볼을 읽지 않는 것이 원인이다.
- **주의:** 이 실패로 빌드 볼륨의 `modules-only.symvers`가 지워졌다. 다음 전체 빌드 때 다시 생성된다.
- `KBUILD_EXTRA_SYMBOLS`로 fimc-is 심볼을 따로 넘기면 `exported twice`로 실패한다. 외부 모듈 빌드에서는 modpost가 `Module.symvers` 전체를 이미 읽기 때문이다.

해결: 외부 모듈(`M=`)로 빌드한다([`Kbuild`](../modules/ov5647-pablo/Kbuild), [`build-module.sh`](../modules/ov5647-pablo/build-module.sh)).

```bash
docker run --rm -v ~/e850-a13-build:/work -v e850-kernel:/work/kernel -v e850-out:/work/out \
    -v "$PWD/modules/ov5647-pablo:/m:ro" e850-build-env:1 bash /m/build-module.sh
```

```text
  CC [M]  /work/out/ext-ov5647/is-cis-ov5647.o
  LTO [M] /work/out/ext-ov5647/is-cis-ov5647.lto.o
  MODPOST /work/out/ext-ov5647/Module.symvers
  LD [M]  /work/out/ext-ov5647/is-cis-ov5647.ko
-rw-r--r-- 1 root root 665456 Oct 30 16:10 /work/out/ext-ov5647/is-cis-ov5647.ko
```

### 4.3 DTB 다시 빌드와 확인

```bash
docker run --rm -v ~/e850-a13-build:/work -v e850-kernel:/work/kernel -v e850-out:/work/out e850-build-env:1 bash -c \
  'export PATH=/work/prebuilts/clang/host/linux-x86/clang-r450784e/bin:$PATH; cd /work/kernel;
   make O=/work/out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- -j10 dtbs'
```

boot.img를 다시 만든 뒤 합친 DTB를 풀어 확인:

```text
modules: 140 copied, 92 autoload            ← is-cis-ov5647.ko 포함(자동 적재는 안 함)
10:	is_sensor_ov5647@36 {
2452:		ov5647@36 {
2456:			compatible = "samsung,exynos-is-cis-ov5647";
(s5krc1s 노드 없음)
```

`.config` 변경은 `CONFIG_CAMERA_CIS_OV5647_OBJ=m` 한 줄뿐이다(변경 전 사본: `/work/out/.config.before-ov5647`).

---

## 5. Debian rootfs 이미지

스크립트: [`tools/linux510/build-rootfs.sh`](../tools/linux510/build-rootfs.sh) — Debian 12 bookworm arm64 minbase + systemd, ssh, i2c-tools, v4l-utils, python3-numpy.

```bash
docker run --rm --privileged -v e850-out:/o:ro -v "$PWD/tools/linux510:/t:ro" -v "$PWD/out:/out" \
    ubuntu:22.04 sh /t/build-rootfs.sh
```

결과(2026-10-31 01:21 KST): `rootfs.simg` 398,062,088 B(sparse, 펼치면 3 GiB ext4).

이미지를 풀어 확인한 내용:

```text
/etc/fstab    : /dev/mmcblk0p24  /  ext4  defaults,noatime  0  1
/etc/hostname : e850-96
/etc/passwd   : root, e850 (uid 1000)        그룹: sudo, video, i2c
/lib/modules/5.10.157-00010-g3b6b9ca51034-dirty : .ko 140개 (is-cis-ov5647.ko 포함)
serial-getty@ttySAC0 : root 자동 로그인
```

- 계정: `e850` / 비밀번호 `e850`(실습용, 네트워크에 연결하기 전에 바꾼다).
- userdata(p24)에 굽는다: `fastboot flash userdata rootfs.simg`. Android 10의 사용자 데이터는 지워진다(백업 대상에서 제외했던 영역).
- 첫 빌드는 마지막 확인용 `du`가 `/proc` 항목 때문에 오류를 내 종료 코드 1로 끝났다(이미지는 정상). 스크립트에서 그 줄을 뺐다.

---

## 6. 다음 순서 (보드 전원 재투입 후)

2026-10-31 01:25 KST 현재 보드는 `fastboot devices`에 보이지만 명령에 응답하지 않는다(시험 1의 중단된 다운로드 이후). eMMC는 바뀌지 않았다.

1. 보드 전원을 껐다 켠다 → Android 10 부팅, `adb devices` 확인.
2. Chrome·ChatGPT 완전 종료, USB-TTL 다시 꽂기, UART 기록 시작(115200 8N1).
3. `adb reboot bootloader` → `fastboot getvar max-download-size`.
4. 크기가 boot.img(55 MB)보다 크면 RAM 부팅:
   ```bash
   fastboot boot out/boot.img          # root= 없음: UART에 busybox 셸이 뜨면 성공
   ```
   셸에서 확인: `dmesg | grep '\[init\]'`, `ls /dev/mmcblk0p*`, `modprobe is-cis-ov5647`.
5. 성공하면 rootfs와 boot를 굽는다(되돌리기: 백업의 `mmcblk0p15.img.gz`를 `fastboot flash boot`).
   ```bash
   fastboot flash userdata out/rootfs.simg
   fastboot flash boot out/boot-root.img   # build-bootimg.sh root=/dev/mmcblk0p24
   fastboot reboot
   ```
6. 크기가 작으면: boot.img를 `img2simg`로 sparse로 바꿔 `fastboot flash boot`로 시험한다(RAM 부팅 대신 eMMC 기록, 백업으로 복구 가능).

이 단계부터는 보드를 직접 다뤄야 하므로 결과가 나오는 대로 이 문서에 이어서 기록한다.

## 7. 전원 재투입 후 확인 (2026-10-31 오전)

Chrome을 ⌘Q로 종료한 뒤 Android 10으로 정상 부팅했다(`adb devices` → `device`).

### 7.1 USB-TTL이 맥에 잡히지 않음

`/dev/cu.usbserial-*`가 없다. USB 장치 목록에도 FTDI(idVendor 0x0403)가 보이지 않는다. 드라이버보다 케이블·포트·어댑터 쪽 문제다.

```bash
ioreg -p IOUSB -l -w0 | grep -E '"USB Product Name"|"idVendor"'
# USB2.0 HUB / Billboard / Flash Card Reader/Writer / Exynos  ← FT232R 없음
```

허브를 거치지 말고 맥 본체 포트에 직접 꽂는다. 다른 케이블로도 바꿔 본다. 꽂았을 때 어댑터 LED가 켜지는지 확인한다.

### 7.2 이 보드의 LK는 모르는 `getvar`에 멈춘다

```bash
adb reboot bootloader
fastboot getvar max-download-size   # max-download-size: 805306368  (768 MB, 0.004 s)
fastboot getvar product             # 응답 없음 → 이후 모든 명령 무응답
fastboot reboot                     # FAILED (usb_read failed with status e00002ed)
```

- 한도는 768 MB다. 55 MB boot.img는 크기 문제가 아니다.
- 온보드 Android 10 LK는 `getvar product`에 응답하지 않고 그대로 멈춘다. Linaro `lk.bin`과는 다른 바이너리다.
- 복구 방법은 **전원 재투입**뿐이다. eMMC는 바뀌지 않는다.
- 시험 1의 멈춤도 같은 종류일 수 있다. 최신 `fastboot` 클라이언트는 `boot`·`flash` 전에 `getvar`(has-slot, partition-type, is-logical 등)를 먼저 보낸다.

**규칙:** 이 LK에는 `getvar max-download-size` 말고는 `getvar`를 보내지 않는다.

다음 시험에서는 `fastboot -v boot`로 실제로 오가는 명령을 남기고, UART로 LK 출력을 함께 기록한다.

### 7.3 시험 2 — RAM 부팅 재시도: LK의 `fastboot boot`는 동작하지 않는다

USB-TTL을 맥 본체 포트에 직접 꽂자 `FT232R USB UART`가 잡혔다. Chrome을 종료한 상태에서 UART를 기록하며 다시 시험했다.

```bash
adb reboot bootloader
fastboot -v boot out/boot.img
# Sending 'boot.img' (54056 KB)    OKAY [  1.352s]
# Booting                           ← 응답 없음
```

UART(LK 로그):

```text
[    4.448082 ] Entering fastboot: reboot bootloader command
[   17.148808 ] fb: download:034ca000      ← 55 MB 수신 성공
[   18.500557 ] fb: boot                   ← 이후 출력 없음, LK 무응답
```

eMMC에서 정상 부팅할 때 LK는 커널로 넘어가기 전에 아래 줄을 찍는다. `fb: boot` 뒤에는 이 줄이 하나도 나오지 않았다.

```text
kernel size: 0x0166600c
ramdisk size: 0x000c212c
DTB size: 0x0001bef4
DTBO: Merge Complete (size:179932)!
updated bootargs: ...
```

**결론:** 이 Android 10 LK는 `boot` 명령을 받기만 하고 실행하지 않는다. 다운로드는 정상이므로 USB나 크기 문제가 아니다. RAM 부팅 대신 boot 파티션에 쓰는 방법으로 바꾼다.

### 7.4 boot 파티션 쓰기 — fastboot 대신 root adb의 `dd`

LK fastboot는 모르는 명령에 멈추는 경우가 많다. 그래서 Android 10(root adb) 안에서 직접 쓴다. boot 파티션은 `mmcblk0p15`이고 크기는 100 MB다.

```bash
adb push out/boot.img /data/local/tmp/boot-linux.img
adb shell 'dd if=/data/local/tmp/boot-linux.img of=/dev/block/mmcblk0p15 bs=1M conv=fsync && sync'
adb reboot
```

되돌리기(Android 10 복구)는 아래 둘 중 하나로 한다. 첫 번째는 Linux initramfs 셸에서 하는 방법이다. 두 번째는 LK fastboot가 응답할 때 쓰는 방법이다.

```bash
# 1) Linux 셸에서
gunzip -c mmcblk0p15.img.gz | dd of=/dev/mmcblk0p15     # 이미지를 보드로 옮길 수 있을 때
# 2) fastboot에서
gunzip -k mmcblk0p15.img.gz && fastboot flash boot mmcblk0p15.img
```

### 7.5 시험 3 — eMMC boot 파티션에서 5.10 커널 첫 부팅: 커널은 OK, init에서 패닉

boot 파티션에 쓰기 전에 현재 내용이 백업과 같은지 해시로 확인한다. 원본은 보드 안에도 한 부 더 둔다.

```bash
adb shell sha256sum /dev/block/mmcblk0p15            # d0fc5688...
gunzip -c mmcblk0p15.img.gz | shasum -a 256          # d0fc5688...  (같음)
adb push out/boot.img /data/local/tmp/boot-linux.img
adb shell 'cp /dev/block/mmcblk0p15 /data/local/tmp/boot-android10.img
           dd if=/data/local/tmp/boot-linux.img of=/dev/block/mmcblk0p15 bs=1M conv=fsync && sync'
adb shell 'head -c 55353344 /dev/block/mmcblk0p15 | sha256sum'   # 2361b615... = 맥의 boot.img
adb reboot
```

UART에서 확인한 결과, LK가 새 이미지를 읽고 5.10 커널로 넘어갔다. bootargs도 DTB `/chosen`에서 가져왔다.

```text
[    6.089455 ] kernel size: 0x02cc0944
[    6.292637 ] bootargs: console=ttySAC0,115200n8 earlycon=exynos4210,mmio32,0x13820000 maxcpus=1 ...
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd051]
[    0.000000] Linux version 5.10.157-00010-g3b6b9ca51034-dirty ... clang version 14.0.7
[    0.000000] Machine model: Samsung ERD3830 board based on EXYNOS3830
[    0.000000] earlycon: exynos4210 at MMIO32 0x0000000013820000 (options '')
[    2.144356] Warning: unable to open an initial console.
[    2.161877] Run /init as init process
[    2.211197] request_module fs-devtmpfs succeeded, but still no fs?
[    2.548861] Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000100
[    2.648924] Reboot failed -- System halted
```

**원인 세 가지**

| 증상 | 원인 | 고침 |
|---|---|---|
| `still no fs?` | GKI 설정에 `# CONFIG_DEVTMPFS is not set`. Android는 init이 `/dev`를 직접 만든다 | 커널에 `CONFIG_DEVTMPFS=y`, `CONFIG_DEVTMPFS_MOUNT=y`. 옛 커널에서도 동작하도록 init에 tmpfs, `mknod`, `mdev` 대안을 넣음 |
| `unable to open an initial console` | initramfs에 `/dev/console`이 없음 | cpio에 `mknod /dev/console c 5 1` |
| init 종료 → 패닉 | `/tmp`가 없어서 `modprobe ... 2>/tmp/err` 리다이렉트가 전부 실패. 모듈이 하나도 안 올라가 `/dev/ttySAC0`도 없음 → `exec sh`가 EOF로 종료 | `/tmp`를 tmpfs로 마운트. init은 끝나지 않는 루프로 바꿈 |
| 재부팅 대신 정지 | 리부트 드라이버(`exynos-reboot.ko`)가 모듈인데 적재 전이었음 | 1단계 모듈로 가장 먼저 올림 |

**교훈:** 이 상태가 되면 보드는 전원 재투입 말고 되돌릴 방법이 없고, 다시 켜도 같은 이미지로 부팅해 또 멈춘다. 그래서 다음 이미지에는 아래 안전장치를 넣었다(7.6).

### 7.6 원격으로 다룰 수 있게 하는 안전장치

1. **2단계 모듈 적재**
   - 1단계(`tools/linux510/modules.stage1`): 리부트, 클록, 타이머, UART, pinctrl, I2C, PMIC, eMMC.
   - 그다음 UART로 `press Enter within 5 s for a rescue shell`을 띄우고 5초 기다린다.
   - Enter를 누르면 나머지 모듈을 올리기 **전에** 셸에 들어간다. 2단계 모듈 중 하나가 커널을 죽이더라도 다음 부팅에서 막을 수 있다.
2. **`restore-android`**
   - initramfs 안에 백업 `mmcblk0p15.img.gz`(Android 10 boot, 9.9 MB)가 들어 있다.
   - 이 명령 하나로 boot 파티션을 원본으로 되돌리고 재부팅한다.
3. **`to-fastboot`**
   - misc 파티션의 BCB command에 `bootonce-bootloader`를 쓰고 재부팅한다.
   - LK가 이것을 읽고 fastboot에서 멈춘다. `adb reboot bootloader`와 같은 원리이고, LK 로그의 `Recovery: command: 32 bootonce-bootloader`가 그 증거다.
4. **`part-dev NAME`**
   - GPT 파티션 이름(`boot`, `misc`, `userdata`)으로 장치 경로를 찾는다.

boot.img를 만들 때는 백업 폴더를 `/b`로 넘긴다.

```bash
docker run --rm -v e850-out:/o:ro -v "$PWD/tools/linux510:/t:ro" -v "$PWD/out:/out" \
    -v ~/Desktop/E850_Android10_backup_20261031:/b:ro ubuntu:22.04 sh /t/build-bootimg.sh
```

### 7.7 `fbmini` — 필요한 명령만 보내는 fastboot 클라이언트

`fastboot flash`는 쓰기 전에 `getvar`(has-slot, is-logical 등)를 여러 번 먼저 보낸다. 7.2절에서 보았듯이 이 LK는 모르는 질문에 멈춘다. 그래서 `download`, `flash`, `reboot`만 보내는 클라이언트를 libusb로 만들었다. 맥에는 Homebrew libusb가 이미 있다.

```bash
cd tools/linux510
clang -O2 -o fbmini fbmini.c -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib -lusb-1.0
./fbmini getvar max-download-size
./fbmini flash boot ../../out/boot.img
./fbmini flash userdata ../../out/rootfs.simg      # sparse 그대로 (768 MB 한도 안)
./fbmini reboot
```

### 7.8 멈춘 보드를 fastboot로 들어가게 하기

LK는 부팅할 때마다 Volume Up 키를 10번 읽는다(`Volume up key: 1` × 10, 1 = 안 눌림). **Volume Up을 누른 채 전원을 넣으면** fastboot로 들어갈 것으로 예상한다. 아직 확인하지 않았다.

이 방법이 안 되면 Exynos USB 다운로드 모드를 쓴다(SW1 DIP 1번 ON, Linaro dltool의 `smdk-usbdl`). 이 경우 RAM에서 Linaro LK가 뜨고, 그 LK의 fastboot로 boot를 다시 쓴다.

### 7.9 DEVTMPFS를 켠 커널 다시 빌드

`build.sh` 없이 `make`를 직접 부를 때는 `build.config.erd3830_t`가 넘겨주던 변수를 직접 넣어야 한다.

- `TARGET_SOC`가 없으면 `drivers/staging/nanohub/ipc_chub.h: fatal error: '.h' file not found`로 실패한다.
- `KCFLAGS`가 빠지면 모든 파일이 다른 플래그로 다시 컴파일된다(첫 시도에서 43분 걸림).

```bash
# e850-build-env:1 컨테이너 안 (/work/out = 빌드 출력)
cd /work/out
/work/kernel/scripts/config --file .config -e DEVTMPFS -e DEVTMPFS_MOUNT   # 전: .config.before-devtmpfs
make -C /work/kernel O=/work/out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C /work/kernel O=/work/out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
     TARGET_SOC=s5e3830 KCFLAGS=-D__ANDROID_COMMON_KERNEL__ -j$(nproc) Image modules dtbs
```

- `.config` 차이는 `CONFIG_DEVTMPFS=y`, `CONFIG_DEVTMPFS_MOUNT=y` 두 줄뿐이다.
- 빌드는 성공했다(17분).
- 이번 전체 빌드에서 `is-cis-ov5647.ko`도 트리 안에서 함께 빌드됐다(`modules.order`에 포함). 예전 트리 밖 모듈(`ext-ov5647`)은 새 `fimc-is.ko`와 맞지 않으므로 치웠다.

### 7.10 루트 장치를 이름으로 찾기

5.10에서 SD 컨트롤러가 먼저 잡히면 eMMC가 `mmcblk1`이 될 수 있다. 그래서 번호 대신 이름을 쓴다.

- 커널 인자 `root=PARTLABEL=userdata`: init이 `part-dev`로 GPT 이름을 장치로 바꾼다.
- rootfs fstab `LABEL=rootfs`: `mke2fs -L rootfs`로 붙인 레이블.

```bash
docker run --rm -v e850-out:/o:ro -v "$PWD/tools/linux510:/t:ro" -v "$PWD/out-root:/out" \
    -v ~/Desktop/E850_Android10_backup_20261031:/b:ro ubuntu:22.04 sh /t/build-bootimg.sh root=PARTLABEL=userdata
```

| 산출물 | 크기 | SHA-256 앞부분 |
|---|---|---|
| boot.img (셸만) | 64,772,096 | `19bb249c` |
| boot.img (`root=PARTLABEL=userdata`) | 64,772,096 | `2434898b` |
| rootfs.simg | 398,295,560 | e2fsck 오류 없음, `/lib/modules/5.10.157-00010-g3b6b9ca51034-dirty` |

initramfs 안의 `restore/boot-android10.img.gz`를 풀면 원본 boot와 같은 `d0fc5688...`이 나온다.

> 실수 기록: 실행 중인 `sh` 스크립트를 편집하면 안 된다. sh는 스크립트를 읽어 가며 실행한다. 빌드 중에는 스크립트 사본(`cp -R tools/linux510 /tmp/tsnap`)을 마운트한다.

### 7.11 다음 순서 (보드 전원 재투입 때)

1. UART 기록을 켠 채로 **Volume Up을 누르고 전원 ON** → UART에서 `Entering fastboot` 확인.
2. `fbmini`로 굽는다.
   ```bash
   ./fbmini flash userdata rootfs.simg
   ./fbmini flash boot out-root/boot.img
   ./fbmini reboot
   ```
3. UART에서 `[init] press Enter within 5 s` → Enter → 구조 셸에서 `ls /dev/mmcblk*`, `lsmod` 확인 → `exit`로 계속 부팅.
4. Debian 로그인(`root` 자동 로그인 또는 `e850`/`e850`).
5. 문제가 생기면 구조 셸에서 `restore-android`.

### 7.12 시험 4 — fbmini로 굽기: boot는 성공, userdata는 LK가 1 GiB FILL에서 멈춤

전원 재투입 때 Volume Up을 10초 누르고 있었더니 LK가 fastboot에 들어갔다. 다만 키 때문은 아니었다. 키는 10번 모두 `1`로 읽혔고, LK가 밝힌 진입 사유는 아래와 같다.

```text
[    9.150847 ] Entering fastboot: Abnormal RST_STAT: 0x10000000
```

굽기 결과:

```text
$ ./fbmini flash boot out-root/boot.img
> download:03dc5800   < DATA03dc5800   < OKAY
> flash:boot          < OKAY                 UART: partition 'boot' flashed
$ ./fbmini flash userdata rootfs.simg
... CHUNK TYPE FILL (lba: 11841560, sct: 1310712 ...)     ← 640 MiB까지는 처리
... CHUNK TYPE FILL (lba: 13152280, sct: 2097144 ...)     ← 1 GiB에서 멈춤, USB도 끊김
```

해결은 `tools/linux510/resparse.py`다. FILL 청크를 32 MiB 이하로 쪼갠다. 풀어 낸 내용은 원본과 SHA-256이 같다(`585c5383...`).

```bash
python3 tools/linux510/resparse.py rootfs.simg rootfs-split.simg   # 35 chunks -> 113
```

### 7.13 시험 5 — 새 init이 0.03초 만에 종료

```text
[    2.299199] Run /init as init process
[    2.354360] Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000100
```

원인은 7.6절에서 추가한 `[ -c /dev/console ] && exec </dev/console ...`였다.

- 노드는 있지만 콘솔 드라이버(`exynos_tty.ko`)가 아직 없어서 open이 실패한다.
- **셸에서 `exec` 리다이렉트가 실패하면 셸 자체가 끝난다.**

고친 방법: 서브셸에서 먼저 열어 보고(`if (exec </dev/console) 2>/dev/null`), 실패하면 `/dev/null`을 쓴다.

검증은 arm64 Docker에서 initramfs를 chroot로 실행해서 했다. 20초 뒤에도 init이 살아 있었고, `[init] ... no root filesystem, starting shell`까지 진행했다.

```bash
docker run --rm --privileged -v "$PWD/out-root:/b:ro" ubuntu:22.04 sh -c \
  'apt-get -qq update; apt-get -qq install -y cpio; mkdir /x && cd /x && gunzip -c /b/initramfs.cpio.gz | cpio -id
   timeout 20 chroot /x /init </dev/null; echo rc=$?   # 124 = 아직 실행 중 = 정상'
```

**규칙:** 보드에 굽기 전에 initramfs를 컨테이너에서 한 번 실행해 본다. 보드에서 한 번 실패할 때마다 전원 재투입이 필요하다.

### 7.14 `rebootarg` — 버튼 없이 fastboot로

LK는 misc BCB를 무시한다. 7.8절 로그에 `Recovery: command: 32 bootonce-bootloader`가 있는데도 그대로 부팅했다. LK가 fastboot로 들어간 것은 재부팅 사유가 `reboot bootloader`였을 때다.

busybox `reboot`는 인자를 넘길 수 없어서 `reboot(2)` RESTART2를 직접 부르는 244바이트 바이너리를 만들었다(`tools/linux510/rebootarg/`). 빌드는 i2crd와 같은 방식이다.

```bash
rebootarg bootloader     # 보드 셸에서
```

### 7.15 시험 6 — 쪼갠 rootfs 굽기 성공, 그런데 사용자 공간이 `ttySAC0`을 여는 순간 보드가 멈춤

```text
[  162.875454 ] partition 'userdata' flashed                     ← resparse.py 덕분에 끝까지 기록
...
[    5.453778] mmcblk0: mmc0:0001 DP6DAB 58.2 GiB
[    5.578094] [init] .../modules.stage1: 14 ok, 0 failed
[    5.588193]  mmcblk0: p1 p5 p12 p13 p14 p15 p16 p17 p18 p19 p20 p21 p22 p24
[    5.604615] [0: busybox: 1] exynos-uart 13820000.uart:  Clock rate : 199875000   ← init이 ttySAC0 open
(이후 UART 출력 없음. 9600~921600 baud 모두 0바이트)
```

- 1단계 모듈은 모두 올라왔고 eMMC 파티션도 정상이다.
- 커널 콘솔로서의 `ttySAC0`은 정상이었다. 4.2초에 `console [ttySAC0] enabled` 이후에도 로그가 이어졌다.
- 그런데 **사용자 공간이 `/dev/ttySAC0`을 여는 순간**(uart startup → threaded IRQ 등록 → set_termios) 출력이 끊기고 시스템이 멈췄다.
- 원래 BSP의 bootargs는 `console=ram`이다. 즉 Android 13 BSP는 이 UART를 tty 콘솔로 쓰지 않는다.

**바꾼 구조** (UART는 커널 로그 전용, 로그인은 USB):

| 항목 | 전 | 후 |
|---|---|---|
| 커널 콘솔 | `console=ttySAC0,115200n8` | `console=ram keep_bootcon` + earlycon. earlycon이 부팅 내내 UART로 출력 |
| `exynos_tty.ko` | 1단계에서 적재 | 적재하지 않음(`autoload.deny`) |
| 셸, 구조 셸 | UART | **USB ACM 가젯** `/dev/ttyGS0` → 맥 `/dev/cu.usbmodem*` |
| 네트워크 | 없음 | **USB NCM 가젯** `usb0`. 보드 192.168.7.1, DHCP로 맥에 주소 배정 → `ssh e850@192.168.7.1` |
| Debian getty | `serial-getty@ttySAC0` | `serial-getty@ttyGS0` (root 자동 로그인), ttySAC0은 mask |
| 멈췄을 때 | 영원히 정지 | **하드웨어 워치독**: busybox `watchdog -T 60 -t 5`. 사용자 공간이 멈추면 60초 뒤 리셋 |
| fastboot 진입 | 버튼, 우연 | `rebootarg bootloader` (`to-fastboot`이 호출) |

USB 가젯은 configfs로 init이 직접 만든다(VID 0x1d6b, PID 0x0104, `acm.usb0` + `ncm.usb0`). 커널 설정에 `CONFIG_USB_CONFIGFS_ACM=y`, `CONFIG_USB_CONFIGFS_NCM=y`가 이미 들어 있다. `switch_root` 뒤에도 가젯은 그대로 남고, Debian의 getty가 `ttyGS0`을 넘겨받는다.

1단계 모듈에 `s3c2410_wdt`, `phy-exynos-usbdrd-super`, `usb_notify_layer`, `usb_notifier`, `s2m_pdic_module`, `dwc3-exynos-usb`를 더했다.

### 7.16 USB 가젯 — E850-96에는 PDIC가 없다 (`modules/e850-vbus`)

```text
s2mu106_charger_set_mode: S2MU106_CHG_CTRL0 = 0xff      ← ERD3830의 충전/PDIC 칩. 이 보드에는 없음
dwc3_gadget_pullup: current_dr_role = 255               ← 컨트롤러가 device 역할로 바뀌지 않음
```

이 커널의 DT는 ERD3830 레퍼런스 보드용이다. 케이블 연결(VBUS)은 S2MU106 PDIC → `usb_notifier` → `dwc3_exynos_vbus_event()`로 들어온다. E850-96에는 이 칩이 없어서 이벤트가 오지 않고, DWC3는 클록이 꺼진 채 남는다.

`e850-vbus.ko`를 만들었다. 적재되면 `dwc3_exynos_vbus_event(dev, true)`를 한 번 부른다. init은 이것을 적재한 뒤 UDC 상태가 `configured`가 될 때까지 기다리고, 그때만 `ttyGS0`을 쓴다.

```text
e850-vbus: VBUS on for 13600000.usb: 0
dwc3 13600000.dwc3: Turn on gadget dwc3-gadget
phy_exynos_usbdrd 135d0000.phy: Turn on LDO
__dwc3_gadget_start ---
dwc3_otg_start_gadget: start check usb configuration timer
```

### 7.17 진짜 원인 — 첫 CPU idle 진입에서 SoC가 멈춘다 (`cpuidle.off=1`)

7.15~7.16의 "UART를 열면 멈춤", "가젯 시작 후 리셋", "루트 마운트 때 리셋"은 모두 같은 현상이었다. 워치독(`watchdog -T 30 -t 200ms`)을 켜면 멈춤이 리셋으로 바뀌고, LK는 `Entering fastboot: Abnormal RST_STAT: 0x1000000`으로 fastboot에 들어간다. 이 덕분에 사람 손 없이 이분 탐색을 할 수 있었다(`cycle.sh`: 빌드 → `fbmini` 굽기 → 재부팅 → 판정).

| 커널 인자 (공통: `e850.nousb e850.noswitch`) | 결과 |
|---|---|
| 2단계 전부 | 모듈 91개 적재 직후 리셋 (11.6초) |
| `e850.stage2=0` | 6.2초에 리셋 (루트 마운트는 성공) |
| `e850.stage2=0 e850.nowdt` | 리셋 없이 멈춤 → 워치독은 원인 아님 |
| `e850.stage2=0 cpuidle.off=1` | **60초 이상 생존** |

init이 일을 마치고 CPU가 처음 idle에 들어가는 순간 멈췄다. 모듈을 적재하는 동안에는 CPU가 바빠서 버텼다. Android에서는 `exynos-cpupm` 등이 idle 상태와 웨이크업을 먼저 설정해 둔다. 여기서는 일단 `cpuidle.off=1`로 idle 진입을 끈다(전력 소모는 늘어남).

시험용 커널 인자 정리:

| 인자 | 뜻 |
|---|---|
| `e850.nousb` | USB 가젯·VBUS 생략 |
| `e850.stage2=N` | 2단계 모듈 앞 N개만 적재 |
| `e850.noswitch` | 루트 마운트 후 Debian으로 넘어가지 않음. 60초 동안 `alive` 로그를 찍은 뒤 `rebootarg bootloader`로 fastboot 복귀 |
| `e850.nowdt` | 워치독 끔 (멈추면 전원 재투입 필요) |

## 8. Debian 12 부팅 성공, 그리고 HDMI 출력

### 8.1 Debian 부팅 (`cpuidle.off=1` 이후)

```text
[   21.825408] [init] switching to /dev/mmcblk0p24
Debian GNU/Linux 12 e850-96 ttyGS0
e850-96 login: root (automatic login)
root@e850-96:~# systemctl is-system-running
running
```

맥에서 접속: `screen /dev/cu.usbmodeme850_961 115200` (USB-C 케이블 하나, root 자동 로그인).

남은 문제 두 가지:

1. **udev가 카메라 모듈을 자동 적재하다 패닉:**
   - `fimc_is` `sensor_module_probe` NULL 접근.
   - 원인은 OV5647 DT의 `power_seq`에 `use = <1>;`가 빠진 것이었다(`sensor dt callback is NULL`). 고쳤다.
   - 그리고 initramfs에서 빼 둔 모듈 49개를 rootfs에서도 `blacklist`에 넣었다(`/etc/modprobe.d/e850-blacklist.conf`, `build-rootfs.sh`가 `autoload.deny`로 생성). 손으로 `modprobe`하는 것은 그대로 된다.
2. **USB 네트워크(NCM/ECM)는 맥 → 보드 방향 수신이 0이다:**
   - 로그에 `workqueue uether ... cpus=7 ... pending: process_rx_w`가 남는다. 수신 작업이 꺼진 CPU 7(`maxcpus=1`)에 걸려 있다.
   - 해결은 다음 과제로 둔다. 셸은 USB 시리얼(ACM)로 충분하다.

**버튼 없이 fastboot로 들어가기** — `to-fastboot`(워치독 데몬을 강제 종료하면 약 20초 뒤 리셋되고, LK가 `Abnormal RST_STAT`로 fastboot에 들어간다):

```bash
root@e850-96:~# to-fastboot         # = sync; kill -9 $(pidof watchdog)
# Mac: fastboot devices → ./fbmini flash boot ... → ./fbmini reboot
```

(5.10의 `reboot bootloader`(PMU SYSIP_DAT0 = 3)는 이 Android 10 LK가 알아듣지 못해 그냥 재부팅된다.)

### 8.2 HDMI: DECON → DSIM0 → LT8912B → HDMI (`modules/e850-hdmi`)

E850-96의 HDMI는 SoC의 MIPI-DSI를 **Lontium LT8912B** 브리지가 HDMI로 바꾼다. 필요한 값은 모두 Android 10의 DTB와 DTBO(idx 2)에서 꺼냈다.

| 항목 | 값 |
|---|---|
| LT8912B | `i2c@13840000`(i2c_1), 0x48(main) / 0x49(DSI·CEC). 칩 ID 레지스터 0x00/0x01 = `0x12 0xb2` |
| 핀 | reset GPA2[2](active low), int GPA2[0], HPD GPA2[4], **GPG1[2] `mipi_switch_hdmi` = high** |
| panel_0 | gpios `<gpg2 5>`, `<gpg1 1>`, regulator `vdd_ldo28` |
| 타이밍 | 1920×1080@60, h-porch 148/88/44, v-porch 36/4/5, 4 lanes, HS 1000 MHz, PMSK 2/308/2/45371 |

5.10 BSP에서 막힌 곳과 해결 (시험 순서대로):

| 증상 | 원인 | 해결 |
|---|---|---|
| DT 오버레이 `FDT_ERR_NOTFOUND` | SoC DT에서 `//#include "s5e3830-dpu.dtsi"`(디스플레이 전체가 주석) | 주석 해제 |
| `MIPI DSIM read Timeout`, `panel is not matched` | LK가 `panel_0`의 `ddi_id`를 **0xff**로 덮어씀(`kd101n65_get_id panel ID is 0xff`) | lcd_info id와 패널 ops id를 0xff로 |
| `panel is not matched`, `finding...`도 없음 | 오버레이 파일 안의 `/ { ... }`는 오버레이 자신의 루트라 적용되지 않음 | `&{/} { ... }` |
| 패널 id가 없을 때 NULL 접근 | `panel_list[10]` 중 2칸만 채워져 있음 | `panel_lt8912b_ops` 등록(DSI 명령 없는 빈 ops) |
| `decon0: ... command mode` | BSP DECON `psr_mode = <2>` | `psr_mode = <0>`(video) |
| DSIM probe가 중간에 반환 | 비디오 모드는 `memory-region`(fb_handover) 필수인데 rmem dtsi에서 주석 | `&{/reserved-memory}`에 `fb_handover`(0xF9800000, 40 MB) |
| `PMSK(36 4 5 0)` | `CONFIG_EXYNOS_DSIM_DITHER`라서 pmsk를 **14칸** 읽음 | 14칸으로 |
| 언더런 irq에서 NULL(`dsim_irq_handler`) | LK가 DSIM을 켜 둔 채로 넘김 → `dsim->panel` 설정 전에 irq 발생 | `if (dsim->panel && ...)` 1줄 패치 |
| `/dev/fb0` write → `ENODEV` | Exynos fb는 write 미지원 | `mmap` |
| fd를 닫으면 화면이 꺼짐 | `decon_release` → `decon_disable` | `fb-hold.py`가 fd를 열어 둔 채 대기(systemd 서비스) |
| 모니터 "No HDMI" | DSI EoT 패킷 사용 중(`eotp enabled`) | lcd_info에 **`eotp_disabled = <1>`** (메인라인 LT8912B는 `MIPI_DSI_MODE_NO_EOT_PACKET`) |

LT8912B 레지스터는 메인라인 `drivers/gpu/drm/bridge/lontium-lt8912b.c`(v6.6) 순서를 그대로 쓴다: init → lanes → mipi basic → video setup(1080p) → dds → rxlogicres → lvds. 사용자 공간에서 `i2cset`으로 쓴다(`tools/linux510/lt8912b-hdmi.sh`).

부팅하면 자동으로 켜진다(`e850-hdmi.service`):

```text
ExecStartPre = modprobe dpu
ExecStart    = fb-hold.py          # /dev/fb0 unblank + 컬러바 + 열어 둔 채 대기
ExecStartPost= lt8912b-hdmi.sh     # DSI 클록이 돈 뒤 브리지 설정
```

**결과: HDMI 모니터에 1920×1080 컬러바 출력 확인 (2026-11-01).**

```bash
# 직접 해 보기
systemctl status e850-hdmi
lt8912b-hdmi.sh status          # 칩 ID, HPD
fb-test.py                      # 다른 그림을 그리려면 fb-hold.py를 참고(mmap + FBIOPAN_DISPLAY)
```

적용 방법(커널 트리): `sh modules/e850-hdmi/apply.sh /work/kernel` → `make ... modules dtbs`.

## 9. 진행 중: HDMI 로그인 프롬프트와 OV5647 캡처

### 9.1 HDMI에 로그인 프롬프트 (VT + fbcon)

GKI 설정에는 `# CONFIG_VT is not set`이 들어 있다. 그래서 프레임버퍼 콘솔(fbcon)과 `tty1`이 없다. 아래 설정을 켜고 다시 빌드한다(전: `.config.before-vt`). Debian에는 `getty@tty1`이 기본으로 켜져 있어서, 이 커널이면 HDMI에 `e850-96 login:`이 뜰 것으로 예상한다.

```text
+ CONFIG_VT=y  CONFIG_VT_CONSOLE=y  CONFIG_HW_CONSOLE=y  CONFIG_VT_HW_CONSOLE_BINDING=y
+ CONFIG_CONSOLE_TRANSLATIONS=y  CONFIG_DUMMY_CONSOLE=y
+ CONFIG_FRAMEBUFFER_CONSOLE=y   (fb.ko 안에 fbcon.o가 들어간다. CONFIG_FB=m)
+ CONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y  CONFIG_FONT_8x8=y (CONFIG_FONT_8x16=y는 원래 있음)
```

```bash
scripts/config --file .config -e VT -e CONSOLE_TRANSLATIONS -e VT_CONSOLE -e HW_CONSOLE \
    -e VT_HW_CONSOLE_BINDING -e DUMMY_CONSOLE -e FRAMEBUFFER_CONSOLE -e FRAMEBUFFER_CONSOLE_DETECT_PRIMARY
make ... olddefconfig && make ... Image modules dtbs      # 공통 헤더가 바뀌어 대부분 재컴파일
```

### 9.2 OV5647: 모듈 적재까지 확인

```text
root@e850-96:~# modprobe fimc-is              # 패닉 없음 (power_seq use = <1> 이후)
[@][SS1:D] is_sensor_probe(0)                  ← 우리 센서 인스턴스 1 (CSI1)
exynos-is-sensor: probe of 14400000.is_sensor0 failed with error -22   ← ERD의 다른 슬롯, 무시
root@e850-96:~# modprobe is-cis-ov5647
[@]cis_ov5647_probe: OV5647 bound to sensor instance 1, addr 0x36
/sys/bus/i2c/devices/7-0036/driver -> .../i2c/drivers/OV5647
```

센서 영상 노드: `/dev/video102` = `exynos-is-ss1` (VC 노드: video214~217 `ssdevice_idvc0~3`).

캡처를 하려면 `S_INPUT` → `S_FMT`(MPLANE) → `REQBUFS`(MMAP) → `QBUF` → `STREAMON` 순서로 호출해야 한다.

- **`S_INPUT` 값**(`CONFIG_USE_SENSOR_GROUP=y`, `is-video.h`):
  - 비트 구성: scenario[31:26] stream[25:24] **position[23:16]** **vindex[15:8]** intype[7:4] **leader[3:0]**
  - position 1(FRONT), vindex 2(`IS_VIDEO_SS1_NUM`), leader 1 → **`0x10201`**
- **센서 찾기:** `is_sensor_s_input()`은 position 1(FRONT)의 센서를 `exynos_is/vender/front_sensor_id`로 찾는다. ERD 카메라 dtsi를 OV5647 dtsi로 바꾸면서 이 노드도 빠졌다. 그래서 OV5647 dtsi에 `front_sensor_id = <SENSOR_NAME_OV5647>`(219)를 넣었다.
- **픽셀 형식:** `V4L2_PIX_FMT_SBGGR10`. 이 BSP는 한 줄을 `ALIGN(w*5/4, 16)` 바이트(packed 10-bit)로 잡는다.

다음: VT 커널과 vender DT로 다시 굽고 `v4l2-ctl -d /dev/video102 -i 0x10201 --set-fmt-video=... --stream-mmap`.

### 9.3 HDMI 텍스트 콘솔 — fbcon이 DECON에서 동작하게 하기

VT를 켠 첫 부팅에서 패닉이 났다.

```text
Console: switching to mono frame buffer device 240x67
Kernel panic - not syncing: CFI failure (target: 0x0)    fbcon_cursor ← fbcon_fb_registered
```

| 증상 | 원인 | 해결 (`modules/e850-hdmi/apply.sh`) |
|---|---|---|
| CFI failure target 0x0 | Android DECON `fb_ops`에 `fb_fillrect/copyarea/imageblit`이 없음(NULL) | `cfb_fillrect/cfb_copyarea/cfb_imageblit` 연결 |
| modpost: `cfb_*` undefined | `FB_CFB_*`는 select 전용이라 손으로 켠 값을 olddefconfig가 지움 | `EXYNOS_DPU30`이 `select FB_CFB_*` (Kconfig 패치) |
| `cfb_imageblit` NULL 쓰기 | `decon_fb_alloc_memory()`가 vmap → memset → **vunmap + `screen_base = NULL`** | 두 줄 삭제(버퍼는 `system-uncached` 힙이라 캐시 문제 없음) |

결과: HDMI에 `e850-96 login:` 프롬프트가 나온다(2026-11-01 확인). 키보드가 없어서(USB host 꺼짐) tty1도 root 자동 로그인으로 둔다(`getty@tty1.service.d/autologin.conf`). 보드 셸에서 `명령 | tee /dev/tty1`을 쓰면 결과를 모니터로 볼 수 있다.

### 9.4 OV5647 — Linux 드라이버로 칩 ID 읽기 성공

`S_INPUT(0x10201)` 첫 시도:

```text
[M219] pin_ctrl(act(1), pin(96), val(1), nm(cam_gpio high (module power enable))
exynos5-hsi2c 138c0000.hsi2c: HSI2C Error Interrupt occurred(IS:0x00000400, ...)
sensor_ov5647_cis_init: chip ID read failed (-6)
```

원인은 **CAM_VDD_33(PMIC LDO32)이 꺼져 있었던 것**이다. Android 10에서는 이미 켜져 있었는데(그래서 CAM_GPIO만 올려도 칩 ID가 읽혔음), Linux에서는 쓰는 곳이 없어서 꺼진다. `regulator_summary`에서 확인한 값은 `vdd_ldo32 state=disabled uV=2800000`이었다. 게다가 전압도 다르다. Android 10 DT는 3.3 V(`0x325aa0`), ERD PMIC dtsi는 2.8 V다.

수정 (`s5e3830-e850-ov5647-camera.dtsi`):

```dts
&l32_reg { regulator-min-microvolt = <3300000>; regulator-max-microvolt = <3300000>; };
power_seq / scenario_normal_on:
    10 vdd_ldo32 on (PIN_REGULATOR 1 2000) → 20 cam_gpio low → 30 cam_gpio high (20 ms) → 40 on_i2c
scenario_normal_off: off_i2c → cam_gpio low → vdd_ldo32 off
```

```text
[M219] pin_ctrl(act(5), pin(0), val(1), nm(vdd_ldo32)
[M219] pin_ctrl(act(1), pin(96), val(0), nm(cam_gpio low)
[M219] pin_ctrl(act(1), pin(96), val(1), nm(cam_gpio high (module power enable))
[M219] pin_ctrl(act(6), pin(0), val(1), nm(on_i2c)
sensor_ov5647_cis_init: OV5647 chip ID 0x5647            ← Linux 5.10 Pablo 드라이버에서 성공
[MOD:OV5647] sensor_module_init(0)
[SS1:D] is_sensor_s_input(in:1, sce:0)(pos:1)-0
```

다음: `S_FMT`(640×480 SBGGR10) → `REQBUFS` → `STREAMON`으로 RAW 프레임 받기.

### 9.5 OV5647 RAW 캡처 — 어디까지 왔나 (`tools/linux510/ov5647-cap.py`)

`v4l2-ctl`은 쓸 수 없다. 노드가 ENUM_FMT를 구현하지 않아 `BG10`을 거부한다. 그래서 ioctl을 직접 부르는 도구를 만들었다. 순서와 이 BSP에서 알아낸 것:

| 단계 | 결과 / 알아낸 것 |
|---|---|
| `S_INPUT` | `(scenario<<26) \| (1<<16) \| (2<<8) \| 1`. position 1, vindex 2 = SS1, leader. 전원 시퀀스와 칩 ID `0x5647` 확인 |
| `S_CTRL V4L2_CID_IS_S_SENSOR_SIZE` (0x009a1045) | 센서 모드는 S_FMT가 아니라 이 값(`(w<<16)\|h`)으로 찾는다 → `sensor mode(640x480@60) = 0 (lane:2)`, `mode_change: 640x480 RAW10` |
| `S_FMT` | 센서 노드는 group **leader**라서 버퍼 type이 `OUTPUT_MPLANE`(10)이다 |
| `REQBUFS`/`QUERYBUF` | 플레인 2개: 이미지 384000 B(640×480×10bit), 메타 51200 B |
| `QBUF` | MMAP이어도 Pablo는 플레인을 dma-buf fd로 다룬다 → `/dev/dma_heap/system-uncached`에서 할당해 `DMABUF`로 넘긴다 |
| `STREAMON` (scenario 0) | **실패**: `is_hw_camif_cfg: ischain is null`, `is_group_buffer_queue: BUG(!sensor)` |
| `STREAMON` (scenario 1, VISION = stand-alone) | **커널 Oops**: `is_groupmgr_init` ← `is_devicemgr_binding` (이 BSP에서는 stand-alone 경로가 깨져 있음) |

**원인:** OV5647은 ISP가 필요 없는 RAW 센서이고, 하드웨어 경로(CSIS → WDMA → 메모리)도 있다. 그런데 Pablo 프레임워크는 센서 그룹 뒤에 ISP 체인(ischain, 3AA)이 연결돼 있다고 가정한다(`group->device->sensor`, camif의 CSIS→IPP mux).

**다음 할 일(둘 중 하나):**

1. ischain 없이 bypass하도록 fimc-is를 최소한으로 패치한다.
   - `is_hw_camif_cfg`: ischain이 없으면 OTF mux를 건너뛴다.
   - `is_group_buffer_queue`: `group->device->sensor`가 없으면 센서 그룹의 sensor를 쓴다.
   - 그 밖에 is_sensor_start 쪽도 손봐야 한다.
2. 아니면 3AA 노드도 열어 ischain을 연결한다. ISP 펌웨어(`is_lib.bin`)가 필요할 수 있다.

DT 변경(이번에 추가): `exynos_is/vender/front_sensor_id = 219`, LDO32 3.3 V + power_seq, VISION scenario용 power_seq.

### 9.6 ISP 없이(bypass) 센서 그룹 돌리기 — `modules/ov5647-pablo/bypass.sh`

OV5647은 ISP가 필요 없는 RAW 센서다. 경로는 CSIS(MIPI 수신) → WDMA → 메모리다. 그런데 Pablo는 센서 그룹 뒤에 ISP 체인(ischain)이 있다고 가정해서, 아래 다섯 곳을 고쳤다. stand-alone(VISION) 모드는 이 BSP에서 `is_groupmgr_init`이 Oops를 내서 쓰지 않는다.

| # | 위치 | 증상 | 패치 |
|---|---|---|---|
| 1 | `is_group_buffer_queue` | `BUG(!sensor)` | `device->sensor`가 없으면 `group->sensor` |
| 2 | `is_hw_camif_cfg` | `ischain is null` → back_start -ENODEV | ischain이 없으면 CSIS→3AA OTF mux 설정 생략 |
| 3 | `is_sensor_s_input` | `is_group_start: BUG(!group->device->sensor)` | `group->device->sensor = sensor` (반대 방향 `sensor->ischain`은 NULL 유지 → ISP 펌웨어 호출 생략) |
| 4 | `is_sensor_s_input` | `is_sensor_shot` NULL(`group->head`) | 센서 그룹 혼자 `head = tail = group` |
| 5 | `tasklet_csis_str_otf` | 프레임 시작 tasklet에서 NULL+0x20 | ischain이 없으면 센서 그룹의 `smp_trigger`만 처리 |

사용자 공간(`ov5647-cap.py`)에서 알아낸 규칙:

- **`V4L2_CID_IS_S_STREAM`(0x009a100e) = 1:** STREAMON은 CSIS/DMA 쪽(back start)만 켠다. 센서 스트리밍(front start)은 이 컨트롤로 켠다.
  ```text
  Sensor Stream On ... [CSI1] settle(640x480@60) = 15, speed(275 Mbps), 2 lane ... [CSI1][VC0] size(640x480)
  sensor_ov5647_cis_stream_on: stream on        ← 프레임 카운트 F2, F3, ...가 약 16 fps로 증가
  ```
- **메타 플레인의 shot magic:** `camera2_shot_ext.shot.magicNumber`는 메타 플레인 오프셋 `0xb38 + 0xbc98 = 51152`에 있고, 값은 `0x92345678`(v8_20_0)이다. 오프셋은 `llvm-dwarfdump --name=camera2_shot_ext fimc-is.ko`로 구했다. 이 값이 없으면 `shot magic number error`로 프레임이 취소된다.
- **`STREAMON`을 먼저 하고 그다음 `QBUF`:** STREAMON 전에 큐에 넣은 버퍼는 vb2가 `start_streaming`보다 먼저 드라이버로 넘긴다. 그러면 그 shot들이 `is_group_start()`보다 먼저 실행되고, `is_group_start()`가 대기 중인 세마포어(`smp_resource`)를 `sema_init()`으로 다시 초기화한다. 그 결과 그룹 스레드가 영원히 깨어나지 못한다. `/proc/<is_gw13>/stack`의 `down_interruptible ← is_group_shot`과, 양쪽에서 찍은 세마포어 주소가 같다는 것으로 확인했다.
- **OUTPUT 큐 완료 신호:** 완료된 버퍼는 POLLOUT(쓰기 가능)으로 알린다.

**현재 상태:** DQBUF로 버퍼가 약 15 fps로 돌아온다. 그러나 이미지 플레인에는 데이터가 없다(시작 표시값 0x5a 그대로). Pablo에서 RAW(VC0)는 leader(`/dev/video102`)가 아니라 **VC 캡처 노드**(`ssdevice_idvc0` = `/dev/video214`)로 DMA된다. 다음은 그 노드를 열고, leader 메타의 `node_group.capture[]`에 VC0 요청을 넣는 일이다.

보드에 빠르게 파일을 보내는 도구(raw tty + `head -c`, 7.6 MB를 5초에 전송): 맥에서 `python3 acmpush2.py fimc-is.ko /tmp/fimc-is.ko`.

### 9.7 첫 RAW 프레임 — VC0 캡처 노드, 테스트 패턴, 실제 영상

Pablo에서 RAW(VC0)는 leader 버퍼가 아니라 **VC 캡처 노드**로 DMA된다.

- 센서 인스턴스 1의 VC0 노드는 `/dev/video214`다(`IS_VIDEO_SS1VC0_NUM` = 110 + 4 = 114, 이름은 `ssdevice_idvc0`).
- 노드 타입은 `CAPTURE_MPLANE`다.
- leader 메타의 `shot_ext.node_group`(@0x58)에서 `capture[0]`(@0x58+0x30)에 `{vid 114, request 1, crop 0,0,640,480, BG10}`을 넣는다.
- 구조체 오프셋은 모두 `fimc-is.ko`의 DWARF(`llvm-dwarfdump`)에서 읽었다.

| 단계에서 만난 문제 | 원인 | 해결 |
|---|---|---|
| `SysMMU WRITE PAGE FAULT at 0xf8dde040` (CSISX2), 이어서 `CSIS_ERR_DMA_DMAFIFO_FULL` 패닉 | VC0 DMA가 RAW10을 **픽셀당 16비트**(LE, 10비트 유효)로 쓴다. 640×480×2 = 614400 B인데 플레인은 packed 기준 384000 B | dma-buf를 플레인의 2배로 할당한다(IOMMU는 dma-buf 전체를 매핑함) |
| `csi_dma_tag`: `WARN_ON(!f_subdev)` 뒤 Oops | 프레임 시작 때 `csis_check_vc_dma_buf`가 PROCESS로 옮긴 VC0 프레임에는 태그(`frame->subdev`)가 없음 | bypass 패치 6: DMA 채널 subdev를 쓰고, leader 프레임은 알 때만 갱신 |
| 경고 백트레이스 폭주 → 워치독 리셋 | 위 경우마다 `WARN_ON(!ldr_framemgr)`가 115200 bps earlycon으로 출력됨 | 태그 없는 프레임이면 경고 생략 |
| 스트림당 VC0 3장만 나옴 | 그룹 shot 자원(`smp_resource` = async 1 + sync 2)은 leader 프레임 완료 때 반납되는데, bypass에서는 leader가 완료되지 않음(완료는 원래 3AA 몫) | 지금은 스트림당 3장으로 쓴다. 프레임 끝에서 leader를 완료시키는 패치는 SoC 멈춤을 일으켜 적용하지 않음 |
| 받은 3장이 모두 검은 화면 | 모드 테이블이 이미 스트리밍을 시작하고, 노출은 수동 `0x000200`(약 2줄)이다. 3장이 `stream_on`보다 먼저 지나감 | `is-cis-ov5647`: 모드 테이블 직후 노출 1000줄, 게인 0x20, AEC/AGC 자동(0x3503=0) 설정(모듈 인자 `aec`, 기본 켬) |

**테스트 패턴**(`modprobe is-cis-ov5647 test_pattern=1`, 0x503D = 0x80)으로 경로 전체를 검증했다. 가로 방향으로 흰·노랑·시안·초록·마젠타·빨강·파랑·검정 8색이 정확히 나온다. CSIS 수신, DMA, 16비트 해석, BGGR 순서가 모두 맞다는 뜻이다.

![OV5647 colour bars through CSIS VC0 DMA](img/ov5647-linux510-testpattern.png)

**첫 실제 프레임**(640×480, 어두운 방, AEC, 초점 조정 전). 값 범위는 0~93/1023이고 `raw10-view.py`가 자동으로 밝기를 늘렸다.

![first OV5647 frame on Linux 5.10](img/ov5647-linux510-first-frame.png)

직접 해 보기 (보드 셸):

```bash
modprobe fimc-is && modprobe is-cis-ov5647          # test_pattern=1 이면 컬러바
ov5647-cap.py -n 3                                   # /tmp/ov5647_0..2.raw (VC0, u16 per pixel)
raw10-view.py /tmp/ov5647_2.raw                      # HDMI 가운데에 2배로 표시 + /tmp/ov5647_2.ppm
```

맥으로 가져오기: `python3 tools/linux510/acmpull.py /tmp/ov5647_2.ppm out.ppm`. 보드에서 맥으로 한 번에 크게 보내면 데이터가 빠지므로, gzip 뒤 16 KiB 조각마다 md5로 확인한다.

**남은 일:**

1. leader 프레임 완료 처리(연속 스트리밍).
2. 같은 부팅에서 스트림을 여러 번 열고 닫을 때의 CSIS 오류(`CSIS_ERR_LOST_FS_VC`) 뒤 리셋.
3. 노출과 프레임 길이(VTS) 조정, 더 큰 모드(1296×972, 1920×1080).

### 9.8 연속 스트리밍과 HDMI 실시간 영상 (`bypass.sh` 6b·7·8, `ov5647-cap.py --live`)

9.7까지는 스트림 한 번에 3장에서 멈췄다. 원인은 두 가지였고, 각각 패치로 풀었다.

**① leader 프레임이 끝나지 않는다 → 패치 7.** 프레임 끝(`is_sensor_notify_by_fend`)에서 leader 버퍼를 직접 완료한다.

- 원래 코드는 `is_group_done()`을 부른다. 이 함수는 ischain 그룹과 메타데이터를 건드린다.
- bypass에서는 이 함수를 부르지 않고 꼭 필요한 두 가지만 한다.
  - shot 카운트를 올린다.
  - `gtask->smp_resource` 세마포어를 돌려준다.

**② VC0 버퍼가 다시 걸리지 않는다 → 패치 8.** ISP 없는 M2M 경로에서는 센서 **line 인터럽트**(14번째 줄)가 다음 프레임의 VC0 버퍼를 건다.

- 이 경로는 `is_sensor_notify_by_line → is_devicemgr_shot_callback → is_sensor_group_tag → VC0 s_rx_buffer` 순서로 간다.
- 그런데 leader 프레임을 `fcount == line_fcount`인 것으로 찾는다.
- bypass에서는 두 카운터가 어긋난다. 그래서 매번 `(SENSOR LINE IRQ) There's no Shot`가 나고 VC0 DMA가 꺼졌다(`DMA off [0/0/3]`).
- 패치 8은 찾지 못하면 "VC0가 아직 안 걸린, 가장 오래된 처리 중 leader 프레임"을 대신 쓴다.

**③ 1 CPU에서 인터럽트를 놓친다 → 패치 6b.** VC0 서브디바이스에는 `leader`가 없다.

- 그래서 `GET_SUBDEV_FRAMEMGR(NULL)`이 프레임마다 전체 백트레이스와 함께 WARN을 찍었다.
- CPU가 하나뿐이라 그동안 CSIS 인터럽트를 놓쳤다(`interrupt lost`, `start/end overlapped`).
- 패치 6b로 이 WARN이 나지 않게 했다.

캡처 도구도 한 군데 고쳤다. 드라이버는 VC0 프레임을 버릴 때 메타의 `capture[].request`를 0으로 지운다. 그래서 leader 버퍼를 다시 넣을 때마다(QBUF) 메타를 새로 쓴다(`write_shot()`).

결과는 다음과 같다.

```bash
modprobe fimc-is && modprobe is-cis-ov5647
ov5647-cap.py -n 30                 # 30장 연속 (파일 저장, 초당 약 1.5장: 저장이 병목)
ov5647-cap.py --live -n 200 -b 8    # HDMI 실시간: "live: 60 frames, 17.7 fps" ... 200장
ov5647-cap.py --live                # Ctrl-C까지 계속
```

`--live`의 처리 방식은 다음과 같다.

- BGGR 2×2 칸 하나를 RGB 한 점으로 바꾼다(320×240). 화면에는 4배로 키워 1280×960으로 그린다.
- 채널별 LUT에 검정 레벨, 그레이월드 화이트밸런스, 자동 밝기, 감마 2.2를 넣는다. LUT는 30장마다 다시 계산한다.
- numpy만 쓰고, `/dev/fb0` mmap에 직접 쓴다.
- CPU 하나(A55)에서 초당 15~17장이 나온다. 센서는 60 fps로 돌기 때문에 남는 프레임은 버린다.

**남은 문제:**

1. 스트림을 켜는 순간 가끔 SoC가 멈춘다(첫 스트림 4번 중 2번). watchdog이 리셋해 fastboot로 돌아오며, 1 CPU라 마지막 커널 로그가 남지 않는다. 다음 시도는 `watchdog_thresh=4`와 `softlockup_panic=1`로 스택을 남기는 것이다.
2. 같은 부팅에서 두 번째 스트림을 열면 한동안 돌다가 리셋된다.
3. 카메라 모듈이 올라간 채로 재부팅하면 종료 단계에서 멈출 수 있다. 재부팅 전에 `rmmod`하거나 fastboot에서 `fbmini reboot`로 복구한다.

### 9.9 스트림 시작 시 멈춤 해결 (`bypass.sh` 6c, `is-cis-ov5647.c`)

**증상**: 부팅 후 첫 스트림을 켜면 2번 중 1번꼴로 보드가 멈췄습니다. 로그는 `ischain is NULL` 다음에 끊기고,
하드웨어 watchdog 리셋(LK: `Abnormal RST_STAT`) 뒤 fastboot에서 멈췄습니다.
멈추지 않은 경우에도 같은 부팅에서 두 번째 스트림을 켜면 리셋됐고, 영상이 1.5 fps밖에 안 나올 때도 있었습니다.

**원인 찾기**: 멈추지 않은 경우의 UART 로그를 보면 프레임마다 다음 경고가 스택 덤프와 함께 찍혀 있었습니다.

```
WARNING: ... at drivers/media/platform/exynos/camera/is-device-csi_v4.c:747 csi_dma_tag+0x434/0x560
```

747번 줄은 패치 6이 넣은 `WARN_ON(!ldr_framemgr && frame->subdev)`입니다. 패치 8 덕분에 VC0 프레임에 tag가
붙게 되면서, 프레임이 전달될 때마다 이 경고가 발생했습니다(bypass 모드의 VC0 subdev에는 leader가 없기 때문입니다).

스택 덤프 하나는 약 3 KB이고, 115200 bps UART로 내보내는 데 0.25초가 걸립니다. 이 출력이 CSIS 인터럽트 안에서 일어나니
CPU가 1개뿐인 시스템에서는 다른 일을 할 수 없습니다. 그래서
- 영상이 1.5 fps로 떨어지고(STREAMON에서 센서 시작까지도 6.7초),
- 운이 나쁘면 watchdog keepalive 스레드가 제때 돌지 못해 **하드웨어 watchdog이 리셋**합니다.
  리셋은 panic이 아니라서 panic 메시지가 남지 않고, 그래서 "로그 없이 멈춤"으로 보였습니다.

**수정**
1. `bypass.sh` 6c: 이 WARN을 지웁니다. bypass에서는 leader가 없는 것이 정상입니다.
2. 콘솔 로그 레벨을 낮춥니다. `/etc/sysctl.d/90-lockup.conf`에 `kernel.printk = 4 4 1 7`을 넣으면
   경고 이상만 UART로 나갑니다. 전체 로그는 `dmesg`로 볼 수 있습니다.
3. `is-cis-ov5647.c`: 모드 테이블을 쓰고 나면 센서가 곧바로 영상을 내보내는데, CSIS는 0.4초 뒤 stream on 때에야 켜집니다.
   그래서 모드 설정 끝에서 센서를 stream off(MIPI LP-11, `0x4800`/`0x4202`/`0x300d`)로 돌려 두고,
   `cis_stream_on`에서 처음 출력을 시작하게 했습니다.

빌드와 설치:

```sh
# fimc-is.ko: bypass.sh(6c 포함)를 적용한 커널 소스로 modules 빌드 → 보드의 /lib/modules/$(uname -r)/ 에 복사
# is-cis-ov5647.ko: modules/ov5647-pablo/build-module.sh
```

**결과** (2026-11-01)

| 시험 | 결과 |
|---|---|
| 같은 부팅에서 스트림 4회 연속 (300 + 150×3장) | 모두 정상, `WARNING` 0개 |
| 재부팅 → 첫 스트림, 3회 반복 | 3/3 정상 (이전에는 4번 중 2번 멈춤) |
| 카메라 모듈을 올린 채 재부팅 | 3/3 정상 종료 (fastboot에 멈추지 않음) |
| HDMI 실시간 영상 | 평균 약 12 fps (30장마다 9~19 fps 사이를 오감) |

**남은 문제**: 첫 스트림을 끌 때 약 24초가 걸립니다. 드라이버가 남은 요청을 `forcely trigger`로 하나씩 끝냅니다.

## 10. Wi-Fi — 칩 안의 Maxwell(Samsung SCSC) 무선랜

Exynos 850에는 Wi-Fi/BT 코어(WLBT, "Maxwell" mx140)가 SoC 안에 들어 있다. 5.10 커널에는 드라이버가 이미 모듈로 빌드되어 있다(`scsc_mx.ko`, `scsc_platform_mif.ko`, `scsc_wlan.ko`). DT에도 `scsc_wifibt@119C0000` 노드가 있다. 없는 것은 **펌웨어**와 **사용자 공간 도구**뿐이다.

### 10.1 펌웨어: Android vendor 이미지에서 꺼내기 (`tools/linux510/wifi-fw-extract.sh`)

Android 10 백업의 `super`(mmcblk0p12)는 동적 파티션 묶음이다. 그 안의 `vendor` ext4에서 `/etc/wifi`를 꺼낸다(`mx140.bin` 1.5 MB, `mx140/conf/*.hcf`, `slsi_reg_database.bin`).

```bash
sh tools/linux510/wifi-fw-extract.sh ~/Desktop/E850_Android10_backup_20261031 out/
#  lpunpack-min.py: system [(3072000 sectors @2048)], vendor [(614400 sectors @3074048)]
#  -> out/wifi-fw.tgz  (vendor/etc/wifi/...)
```

`lpunpack-min.py`는 LP 메타데이터(slot 0)를 직접 읽는 30줄짜리 파서다. extent가 하나뿐인 선형 파티션만 다룬다.

### 10.2 드라이버가 펌웨어를 찾는 경로

`mx140_file.c`는 `request_firmware("../etc/wifi/mx140.bin")`을 부른다. 즉 `/lib/firmware/../etc/wifi`다. Debian rootfs에는 `/lib/firmware` 자체가 없어서 `..`가 풀리지 않았다(`Direct firmware load ... failed with error -2`). 다음 두 가지로 해결했다.

```bash
tar xzf wifi-fw.tgz -C /              # /vendor/etc/wifi
mkdir -p /lib/firmware /lib/etc
ln -s /vendor/etc/wifi /lib/etc/wifi
```

### 10.3 올리기 (`tools/linux510/e850-wifi.sh`)

```bash
modprobe scsc_mx && modprobe scsc_wlan    # wlan0, p2p0, wlan1 생김 (blacklist는 이름 지정 modprobe에는 안 걸림)
ip link set wlan0 up                      # 이때 mx140.bin을 내려받고 WLBT 코어를 부팅
iw dev wlan0 scan                         # 주변 AP가 보이면 성공
```

`slsi_read_regulatory: Failed to read regdb`와 `WLBTD not running` 경고는 나오지만 스캔은 된다.

사용자 공간 도구는 `wpasupplicant`, `iw`, `libnl-*`, `libpcsclite1`, `wireless-regdb`다. 보드에 아직 네트워크가 없으므로 맥의 `debian:bookworm`(arm64) 컨테이너에서 `apt-get download`로 받는다. 받은 패키지는 `acmpush2.py`로 보내 `dpkg -i`로 설치했다. 접속은 `e850-wifi.sh SSID 암호`로 한다(wpa_supplicant@wlan0 + systemd-networkd DHCP).

### 10.4 접속 성공 — DHCP와 DNS

```bash
e850-wifi.sh TP-Link_36D7 '<암호>'
#  wpa_supplicant: Associated ... WPA: Key negotiation completed [PTK=CCMP GTK=CCMP]
#  inet 192.168.0.236/24 ... dynamic wlan0      (5 GHz, ch149, -70 dBm)
ping -c 2 8.8.8.8 && ping -c 1 deb.debian.org
```

처음 시도에서 걸린 두 가지는 다음과 같다(스크립트에 반영함).

1. **`.network` 파일 권한.** 암호 파일 때문에 `umask 077`을 걸었더니 `20-wlan0.network`도 0600이 됐다. networkd는 `systemd-network` 사용자로 돌아서 `Permission denied`가 나고, wlan0이 `unmanaged`로 남았다. 암호 파일만 0600으로 만든다.
2. **DNS.** minbase rootfs에는 systemd-resolved가 없다. `/etc/resolv.conf`에는 Docker 빌드 때 쓰던 `192.168.65.7`이 남아 있었다. 게이트웨이와 `8.8.8.8`로 바꿨다.

암호는 보드의 `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`에 PSK 해시로만 남고, 저장소에는 넣지 않는다. 이제 보드에서 `apt-get install`이 바로 된다.

### 10.5 부팅할 때 자동 연결 (`tools/linux510/e850-wifi.service`)

`e850-wifi.sh SSID 암호`를 한 번 실행해 접속 정보를 저장한 뒤, 서비스를 설치합니다.
서비스는 `e850-wifi.sh up`을 실행합니다. 이 명령은 드라이버를 올리고 wlan0을 켠 다음,
저장된 네트워크(`/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`)에 다시 접속합니다.
접속 정보가 없으면 서비스는 실행되지 않습니다.

```sh
cp e850-wifi.sh /usr/local/bin/ && chmod 755 /usr/local/bin/e850-wifi.sh
cp e850-wifi.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable e850-wifi
```

확인 결과: 재부팅하고 약 1분 뒤 `systemctl is-active e850-wifi` → `active`.
wlan0은 DHCP로 192.168.0.236을 받았고, `ping 8.8.8.8`이 응답했습니다.

---

## 11. 서비스(SSH·웹)와 CPU 코어 켜기

### 11.1 SSH·SFTP·웹 서버

Wi-Fi(10장)가 되면 Debian arm64 패키지를 `apt`로 바로 받는다. 커널·드라이버와 상관없는 사용자 프로그램이다.

```sh
printf 'deb http://deb.debian.org/debian bookworm main\ndeb http://deb.debian.org/debian bookworm-updates main\ndeb http://security.debian.org/debian-security bookworm-security main\n' > /etc/apt/sources.list
apt-get update && apt-get install -y systemd-timesyncd sudo nginx
useradd -m -s /bin/bash -G sudo <사용자> && passwd <사용자>   # Debian 기본값은 root 비밀번호 SSH 로그인 차단
```

| 서비스 | 확인 |
|---|---|
| SSH / SFTP (`openssh-server`, 이미 설치됨) | 맥에서 `ssh <사용자>@192.168.0.236` → `SSH-2.0-OpenSSH_9.2p1` |
| nginx | 맥에서 `http://192.168.0.236/` → HTTP 200 |
| 시간 동기화 | 보드 시계가 2026년 4월로 시작한다. timesyncd가 없으면 `apt`·HTTPS가 날짜 오류를 낸다 |

FTP(vsftpd)는 비밀번호가 평문으로 가므로 넣지 않았다. 파일 전송은 SFTP를 쓴다.

### 11.2 CPU가 1개만 켜진 이유

`/proc/cmdline`의 `maxcpus=1`은 Samsung DT bootargs에 원래 있던 값이다. 커널은 8개를 인식한다(`possible 0-7`, `online 0`). 부팅 옵션을 바꾸지 않고 실행 중에 코어를 하나씩 켜서 시험한다.

```sh
echo 1 > /sys/devices/system/cpu/cpu1/online
cat /sys/devices/system/cpu/online
```

### 11.3 CPU1 lockup — 원인은 MCT 타이머

| 시도 | 결과 |
|---|---|
| 그냥 켜기 | 몇 초 뒤 `Watchdog detected hard LOCKUP on cpu 1` → 리셋 → fastboot |
| CPU1을 바쁘게 한 상태에서 켜기 | 같음 |
| `exynos-ecc-handler` 제거(IRQ 71) | 같음 |
| `exynos_mct`·`hardlockup-watchdog` 제거, arch_timer만 사용 | **CPU1이 30초 넘게 동작, 타이머 인터럽트 934회** |

lockup 당시 CPU1의 hrtimer 인터럽트 수가 0이었다. MCT 로컬 타이머가 인터럽트를 올려도(`INTCSTAT=1`) CPU1까지 전달되지 않았다. 그래서 tick을 받지 못한 CPU1을 hardlockup 감시가 lockup으로 판정했다. ARM arch timer는 코어마다 PPI로 직접 인터럽트를 받으므로 이 문제가 없다.

반영한 것(세 군데 모두 빼야 한다):

- `tools/linux510/modules.stage1`: `exynos_mct` 줄을 주석 처리. stage1에 이름으로 따로 들어 있어서 deny 목록에 걸리지 않는다.
- `tools/linux510/autoload.deny`: `^exynos-ecc-handler`, `^exynos_mct`, `^hardlockup.watchdog` 추가. 파일 이름이 `hardlockup-watchdog.ko`(대시)라서 `.`으로 맞춘다. `vendor_boot_module_order_s5e3830.cfg`에서 온 항목도 이 목록으로 걸러진다.
- 보드 rootfs: `/etc/modprobe.d/e850-blacklist.conf`에 `blacklist exynos_mct`, `blacklist hardlockup-watchdog`, `blacklist exynos-ecc-handler`. udev가 다시 올리지 않게 한다. `build-rootfs.sh`는 `autoload.deny`로 이 파일을 만들므로 새 rootfs에는 자동으로 들어간다.

검증: 새 boot.img의 initramfs에서 `modules.load`·`modules.stage1`에 `mct|hardlockup|ecc`가 없음을 확인했다. 플래시 후 정상 부팅됐고, `lsmod`에도 셋 다 없었다.

부작용: hardlockup 감시가 없어진다. 멈춤 복구는 `s3c2410_wdt` 대신 쓰는 사용자 공간 `watchdog` 데몬에 맡긴다.

### 11.4 남은 일

1. ~~CPU2~7 켜기~~ — **해결(11.8절). 8코어 모두 동작.**
2. ~~`maxcpus=1` 빼기~~ — 11.8절에서 함께 반영.
3. ~~카메라 fps 다시 재기~~ — 11.10절. 그대로 12~16 fps.
4. ~~장시간 안정성~~ 통과(11.11). 남음: `cpuidle.off=1` 빼기는 실패(11.12, 펌웨어 문제로 보여 보류), ~~hotplug~~ 해결(11.13), ~~카메라 fps~~ 55~63 fps로 해결(11.10).
5. ~~HDMI 글자색~~ 흰색으로 해결(11.14).
6. 다음(사용자 계획, [세션 기록](SESSION-LOG-20261101-02.md) 5절): MySQL + Apache로 출입 기록(nginx와 80번 포트를 어떻게 나눌지 결정 필요), YOLO nano(ncnn/ONNX Runtime, 카메라 → HDMI).

### 11.5 보드를 USB로 연결하면 맥의 인터넷이 끊기는 문제

보드의 USB NCM 가젯은 맥에 `E850-96 Linux`(en13) 네트워크 장치로 잡힌다. 맥의 서비스 순서에서 이 장치가 Wi-Fi보다 위에 있다. 그런데 보드의 networkd DHCP 서버는 기본값(`EmitRouter=yes`)으로 자기(192.168.7.1)를 기본 게이트웨이로 알려 준다. 그 결과 맥의 모든 트래픽이 보드로 가서 인터넷과 DNS가 끊긴다(`ENOTFOUND`).

- 보드: `build-rootfs.sh`의 `10-usb0.network`에 `EmitRouter=no`를 넣었다. 이미 만든 rootfs에는 직접 넣는다.
  ```sh
  sed -i 's/^EmitDNS=no$/EmitDNS=no\nEmitRouter=no/' /etc/systemd/network/10-usb0.network && systemctl restart systemd-networkd
  ```
- 맥(바로 적용): 시스템 설정 → 네트워크 → … → 서비스 순서 설정에서 Wi-Fi를 맨 위로 올린다.

### 11.6 CPU2~4: `failed to come online` (멈춤 없음)

MCT를 뺀 이미지에서 CPU1을 켠 상태(`online 0-1`)로 시험했다.

```text
echo 1 > /sys/devices/system/cpu/cpu2/online   → rc=1
CPU2: failed to come online
CPU2: failed in unknown state : 0x0
```

CPU3·CPU4(다른 클러스터, MPIDR 0x10000)도 같다. 보드는 리셋되지 않고 CPU 0~1로 계속 동작한다.

- DT: 8개 모두 `enable-method = "psci"`이고 MPIDR은 0x0, 0x100, 0x200, 0x300, 0x10000 … 순서로 정상이다.
- `__cpu_up()`(arch/arm64/kernel/smp.c)은 PSCI `CPU_ON`이 성공을 돌려준 뒤 5초를 기다린다. 상태 0x0은 그 코어가 커널 진입점(`secondary_entry`)에 한 번도 들어오지 못했다는 뜻이다. 그러니 커널 쪽 lockup(CPU1 때의 MCT 문제)과는 다른 문제다. 펌웨어(EL3)나 전원 쪽에서 코어가 실제로 켜지지 않는다.
- exynos-cpuhp: `available cpu 0xff`, `set online cpu 0xff`, `cpu_online_mask 0x3`. Samsung hotplug 드라이버가 막고 있지는 않다.

다음에 볼 것: CPU1만 켜지는 이유(부팅 때 LK/EL3가 CPU1만 미리 준비했는지), `exynos-cpupm`의 cluster/core 전원 콜백, Android 10에서 8코어가 켜질 때의 PMU/ACPM 순서.

### 11.7 꺼진 코어에 전원이 들어가지 않는다 (`modules/cpu-pmu-enable`)

`exynos-cpupm`은 probe 때 켜져 있던 CPU에만 `cal_cpu_enable()`(flexpmu wake-up 마스크 `GRP2_INTR_BID_ENABLE` 해제)을 호출한다. `maxcpus=1`이면 CPU0만 해당된다. 보드에 `/dev/mem`이 없으므로, 나머지 CPU에 이 함수를 호출하고 PMU 상태(`cal_cpu_status`)를 찍는 작은 모듈을 만들었다.

```sh
docker run --rm -v ~/e850-a13-build:/work -v e850-kernel:/work/kernel -v e850-out:/work/out \
    -v "$PWD/modules/cpu-pmu-enable:/m:ro" e850-build-env:1 bash /m/build-module.sh
insmod cpu-pmu-enable.ko          # mask=0xfc (CPU2-7) 기본값
# cpu-pmu-enable: cpu2 status 0 -> enable 0 -> status 0
# cpu-pmu-enable: cpu4 status 1 ...   (앞서 CPU_ON을 시도한 뒤라 전원은 켜졌지만 커널에 못 들어옴)
```

| 시험 | 결과 |
|---|---|
| 부팅 후 처음 CPU1 켜기 | 성공 |
| CPU1 끄기(`psci: CPU1 killed`) → 다시 켜기 | 실패, `unknown state 0x0` |
| 부팅 후 처음 CPU2 켜기 | 실패. 그 뒤 CPU1은 성공 |
| `cal_cpu_enable()` 뒤 CPU2 켜기 | 실패. PMU 상태도 계속 0(전원 꺼짐) |

결론: 꺼져 있는 코어를 PSCI `CPU_ON`으로 켜는 경로가 동작하지 않는다. CPU1은 부트로더가 켜 둔 채(WFE 대기) 넘겨준 것으로 보인다. 그래서 첫 번째만 깨어나고, 한 번 끄면 다시 전원이 들어가지 않는다. CPU4는 전원은 켜졌지만 커널 진입점까지 오지 못했다.

다음 후보:
1. Android 10(같은 부트로더)에서는 8코어가 켜진다. Android에서 hotplug 전에 무엇이 다른지 확인한다. 예: `exynos_mct` 로드, ACPM/APM 펌웨어 준비 순서, `cpuidle.off=1` 여부.
2. EL3(BL31)의 `CPU_ON`이 APM(flexpmu)에 전원 요청을 보내는 방식(ACPM IPC 채널)이 Linux 쪽 초기화를 기다리는지 확인한다.
3. 우회: `maxcpus=1`을 빼고 부팅 때 커널이 8개를 한꺼번에 켜게 한다. 부팅 시점에는 부트로더가 준비한 상태라 될 가능성이 있다. MCT를 이미 뺐으므로 CPU1 lockup 원인은 없다.

### 11.8 해결: DT의 CPU `reg`(MPIDR)를 이 보드 BL31 방식으로 (8코어 동작)

`maxcpus=1`만 빼고 부팅해도 결과는 같았다. 그때 로그에서 원인이 보였다.

```text
CPU1: Booted secondary processor 0x0000010000      ← DT의 CPU1 reg는 0x100인데 클러스터1 코어0이 깨어남
CPU2..7: failed to come online
```

| | CPU0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| vendor 5.10 DT (`s5e3830.dtb`) | 0x0 | 0x100 | 0x200 | 0x300 | 0x10000 | 0x10100 | 0x10200 | 0x10300 |
| Android 10 boot.img DT | 0x0 | 0x1 | 0x2 | 0x3 | 0x100 | 0x101 | 0x102 | 0x103 |

이 보드의 BL31(Android 10 시절)은 PSCI `CPU_ON`의 대상을 `클러스터<<8 | 코어`로 해석한다. 그래서 5.10 DT 값으로는 다음과 같이 된다.
- `0x100` → 클러스터1 코어0이 깨어나 논리 CPU1로 동작한다. 그래서 CPU1이 처음 한 번은 켜졌다.
- `0x200`, `0x300` → 없는 클러스터라 아무것도 켜지지 않는다(`unknown state 0x0`).
- `0x10000…` → 해석되지 않는다.

11.3~11.7절의 증상이 모두 이것으로 설명된다. CPU4의 PMU 상태만 1이었던 것도 같은 이유다.

수정(`tools/linux510/build-bootimg.sh`):
- merged.dtb의 `/cpus/cpu@000N/reg`를 Android 10 값으로 바꾼다(`fdtput`).
- 커맨드라인에서 `maxcpus=1`을 뺀다.

결과(부팅 직후):

```text
CPU1: Booted secondary processor 0x0000000100
CPU2: Booted secondary processor 0x0000000200
...
CPU7: Booted secondary processor 0x0000010300
nproc → 8, online 0-7
```

- IPI0/IPI1 카운터가 8개 CPU 모두에서 증가한다.
- 30초 동안 busy loop 8개를 돌리면 CPU 0~7에 고르게 나뉜다. lockup은 없었다.
- Wi-Fi, nginx, USB 네트워크는 그대로 동작한다.
- `modules/cpu-pmu-enable`은 진단용으로 남겨 두었다. 이제는 필요 없다.

참고: 코어가 스스로 읽는 MPIDR(0x100, 0x10000 …)은 DT 값(0x1, 0x100 …)과 다르다. 그래도 IPI는 정상이다. 아직 확인하지 않은 것: `cpuidle.off=1`을 빼도 되는지, 장시간 안정성. hotplug는 11.9절.

### 11.9 CPU hotplug(끄고 다시 켜기)는 아직 안 된다 → 11.13에서 해결

8코어로 부팅한 뒤 시험했다.

```text
echo 0 > cpu2/online   → psci: CPU2 killed (polled 0 ms)
echo 1 > cpu2/online   → CPU2: failed to come online / unknown state 0x0   (CPU5도 같음)
```

- `cpu-pmu-enable.ko mask=0x24`로 깨우기 마스크를 미리 풀어도 같다. 이때 PMU 상태는 1(전원 켜짐)이다. 즉 전원은 들어오지만 코어가 커널 진입점까지 오지 못한다.
- `kvm-arm.mode=none`을 붙여도 같다. 로그에 `KVM is not available`이 나온다. 커널이 EL1로 들어오므로 pKVM은 관련이 없다.
- 부팅 때는 부트로더가 코어를 준비해 둔 상태라 켜진다. `CPU_OFF`로 한 번 꺼진 코어를 BL31이 다시 웜부트시키는 경로가 막혀 있다.

실사용 영향: 부팅 때 8코어가 켜지고, `cpuidle.off=1`이라 코어가 꺼질 일이 없다. 그러니 hotplug를 쓰지만 않으면 문제없다. **`echo 0 > cpuN/online`은 하지 말 것.** 그 코어는 재부팅할 때까지 돌아오지 않는다. (11.13의 `e850-cpuhp.ko`가 올라가 있으면 된다.)

### 11.10 8코어에서 카메라 실시간 영상 fps

```text
ov5647-cap.py --live -n 200 -b 8
live: 60 frames, 16.0 fps / 90: 12.0 / 120: 15.8 / 150: 13.4 / 180: 14.1
```

1코어일 때(15~17 fps)와 거의 같다. `--live`는 Python 스레드 하나에서 디모자이크, LUT, framebuffer 쓰기를 모두 하므로, 코어가 늘어도 빨라지지 않는다. 더 빠르게 하려면 프레임 수신과 변환을 여러 프로세스로 나누거나(`multiprocessing`) C로 다시 짜야 한다.

**재측정과 원인 정정 (2026-11-02 03시).** 위의 "Python 한 스레드가 병목"이라는 판단은 틀렸다.

- 보드에서 `--live` 한 프레임 처리를 단계별로 쟀다. 합계가 약 27 ms(최대 약 37 fps)였다. 가장 큰 것은 4배 확대 `repeat()`(16 ms)였다. 그다음은 uncached DMA 버퍼 읽기(4.7 ms)와 LUT 세 번(3.6 ms)이었다.
- `show()`를 바꿨다. LUT를 XRGB8888 자리로 미리 밀어 둔 `uint32`로 만들고, 세 번 찾아 OR한 뒤, framebuffer를 `(행, s, 열, s)` 모양으로 보고 broadcast로 한 번에 쓴다. 이렇게 하면 LUT+확대+쓰기가 21 ms에서 10 ms로 준다. 화면 결과는 예전과 바이트 단위로 같다(Mac에서 numpy로 비교).
- 그런데 fps는 그대로였다(바꾸기 전 11.5~20, 바꾼 뒤 11.1~16.2). UART 로그의 `[bypass] FS`(프레임 시작) 간격이 대부분 60~65 ms였다. **센서가 초당 약 16장만 내보낸다.** 드라이버 로그에는 `sensor mode(640x480@60)`라고 나오지만, 실제 OV5647 PLL/VTS 설정(`0x3034=0x1a, 0x3035=0x21, 0x3036=0x46`)이 그만큼 빠르지 않다.
- 그래서 더 빠르게 하려면 CPU 쪽이 아니라 센서 레지스터(PLL, HTS/VTS)를 고쳐야 한다. 30 fps 단위로 16과 11.4가 번갈아 나오는 것은 가끔 프레임을 놓치는 것이다. 원인은 아직 모른다.
- 덤으로 알아낸 것: `rmmod fimc_is` 뒤에는 `modprobe fimc-is`가 `Device or resource busy`로 실패한다. 카메라 모듈을 다시 올리려면 재부팅해야 한다. 이 보드에서 `systemctl reboot`는 LK가 `Abnormal RST_STAT: 0x1000000`으로 보고 fastboot에 들어간다. Mac에서 `fastboot reboot`를 하면 정상 부팅한다.

**센서 16 fps의 원인과 해결 (2026-11-02 04시): 이제 55~63 fps.**

- 스트리밍 중에 I2C로 센서 레지스터를 읽었다(`/root/ovreg.py 7 380e 380f ...`, I2C_RDWR라 드라이버가 잡고 있어도 된다). VTS(0x380e/f)가 리셋값 `0x7b0`(1968줄)이었다. 55 MHz ÷ (1852 × 1968) ≈ 15 fps라서 관측과 맞는다.
- mainline `ov5647.c`는 VTS를 모드 표가 아니라 vblank 컨트롤로 쓴다. 그런데 모드 표만 옮긴 Pablo 드라이버에는 그 부분이 없었다. 고친 내용: `sensor_ov5647_cis_mode_change()`가 모드 표 다음에 `ov5647_modes[mode].vts`를 0x380e/f에 쓴다(VGA는 `0x1f8`).
- 그런데 VTS만 고치면 오히려 **4~5 fps**가 된다. 커널 명령줄의 `loglevel=8 ignore_loglevel` 때문에 카메라 드라이버의 프레임마다 나오는 메시지(`[bypass] FS`, `There's no Shot` 등)가 인터럽트 안에서 115200 baud UART로 나간다. 그러면 CSIS가 인터럽트를 놓친다(`interrupt lost`, `start/end overlapped`). 이렇게 망가진 뒤에는 같은 부팅에서 다음 스트림이 프레임 0장이 된다.
- 그래서 `ov5647-cap.py`가 실행되는 동안만 `/sys/module/printk/parameters/ignore_loglevel`을 `N`으로, 콘솔 loglevel을 1로 바꾸고, 끝나면(`atexit`) 원래대로 돌려놓는다. 메시지는 dmesg에는 그대로 남는다.
- 결과(8코어, `ov5647-cap.py --live -n 600 -b 8`을 연속 두 번): **63.1 fps, 55.9 fps.** 두 번째 실행도 정상이다. 같은 조건에서 VTS를 고치기 전은 콘솔이 조용해도 16.0 fps였다(그때는 16과 11.4가 번갈아 나오던 흔들림도 없어졌다).
- 보드에는 새 `is-cis-ov5647.ko`(strip 후 md5 `968bfa3d...`)를 `/lib/modules/$(uname -r)/`에 넣었다. 원래 모듈은 `/root/is-cis-ov5647.ko.orig`에 있다.
- 덤: 이번에는 `systemctl reboot`가 fastboot에 들어가지 않고 바로 다시 부팅한 적도 있었다(두 번 중 한 번). 리셋 원인은 아직 모른다.

### 11.11 장시간 안정성 (통과)

- 대기 상태: 8코어로 부팅한 뒤 45분 동안 LOCKUP, Oops, RCU stall이 없었다. 온도는 33~35℃였다.
- 1시간 부하 시험: 2026-11-02 00:41(KST)에 시작했다. 보드의 `/root/stress1h.sh`가 busy loop 8개를 돌리고, 1분마다 loadavg, online CPU, 온도를 `/root/stress1h.log`에 남긴다. 끝나면 `DONE`이 찍힌다.
- 결과(00:41~01:41 KST): 통과. `DONE 16:41:17`(UTC)까지 60번 기록이 모두 `on=0-7`이었고 loadavg는 8.0~8.7이었다. UART 로그에 LOCKUP, RCU stall, Oops, BUG, panic이 없었다.
- 온도: 5분 만에 85℃에 이르렀고, 그 뒤 83~87℃(최고 87℃)를 유지했다. 세 번째 센서는 최고 72℃였다. 부하를 멈추자 49~50℃로 내려갔다. 방열판이 없어 87℃는 여유가 적다. 계속 부하를 줄 때는 방열을 보강해야 한다.

### 11.12 `cpuidle.off=1` 빼 보기 (2026-11-02 04시): 실패, 부팅 초반에 멈춤

8코어 이미지(`boot-nokvm`)에서 명령줄의 `cpuidle.off=1`만 뺀 `boot-idle`을 구웠다. 빌드는 `tools/linux510`을 복사해 sed로 그 인자만 지웠다.

- 결과: **1.93초에 멈췄다.** UART 마지막 줄은 내장 드라이버 initcall(`usbhid: USB HID core driver`)이었다. 그 뒤로 로그가 늘지 않았고 USB도 나타나지 않았다.
- 워치독 데몬(`watchdog -T 30`)은 initramfs 1단계에서야 켜진다. 그래서 이번 멈춤은 리셋으로 바뀌지 않는다. **전원을 껐다 켜야** 한다. 그러면 LK가 비정상 리셋으로 보고 fastboot에 들어간다(7.12). 그때 `boot-nokvm`을 다시 굽는다.
- 7.17(1코어)에서는 initramfs가 끝나고 첫 idle에서 멈췄다. 8코어에서는 CPU1~7이 부팅 직후부터 idle이라 훨씬 일찍 멈추는 것으로 보인다. 다만 이 시점에는 exynos cpuidle 드라이버(모듈, 4.2초)가 아직 없다. 그러니 idle은 WFI뿐이어야 한다. `cpuidle.off=1`이 있을 때와 무엇이 다른지는 아직 모른다. 한 번만 시험했으므로 재현성도 확인하지 않았다.
- 복구(04:40): 전원을 껐다 켜자 LK가 fastboot에 들어갔다. 미리 띄워 둔 대기 루프(`until fastboot devices | grep -q fastboot; ...`)가 `boot-nokvm`을 다시 구웠다(`fastboot reboot`가 `usb_read failed`를 내도 재부팅은 된다). 다시 8코어, `cpuidle.off=1`로 정상이다.
- hotplug(11.9)와 같은 뿌리로 보인다. 둘 다 코어가 전원을 끄거나 깊은 idle에 들어간 뒤, Android 10 시절 BL31이 그 코어를 다시 깨우는 경로를 거친다.
- 결론: 당분간 `cpuidle.off=1`을 유지한다.

**두 번째 시도 (2026-11-02 05~06시): C2를 허용하면 들어간 코어가 깨어나지 않는다.** 실험 코드는 저장소에 넣지 않았다(Mac `~/e850-logs/e850-cpuhp-c2-experiment.c`).

| 구성 | 결과 |
|---|---|
| `cpuidle.off=1` 제거 + DT C2에 `wakeup-latency-us = 0xf0000000`(governor가 고르지 않음) | 부팅 정상, `psci_idle` 드라이버 동작, WFI만 사용. 1.9초 멈춤은 C2(PSCI `CPU_SUSPEND` 0x10000, 코어 전원 끔) 때문이었다 |
| 모듈로 latency를 230 µs로 되돌림 + cpu_pm notifier에서 hotplug와 같은 PMU 쓰기(INT_EN bit3 포함) | CPU3~7이 C2에서 돌아오지 않음. `rcu_preempt kthread starved ... ->cpu=3`: C2에서 arch timer가 멈춘다 |
| + DT에 `local-timer-stop`(broadcast는 `bc_hrtimer`) | 타이머 문제는 사라졌지만, CPU4가 다른 코어로 보낸 IPI(`smp_call_function_single`)에서 영원히 기다림 → soft lockup |
| notifier 제거(PMU 쓰기는 vendor `exynos-cpupm` 훅만), CPU7 또는 CPU6~7만 허용 | 허용된 코어가 C2에 들어간 뒤 돌아오지 않음(`workqueue lockup - pool cpus=7`). `usage`는 C2에서 나올 때 오르므로 계속 0이었다 |

- Android 10 DT의 C2도 같은 PSCI 값(0x10000)과 latency를 쓴다. 다만 `compatible = "exynos,idle-state"`라서 4.14의 Samsung 전용 cpuidle 드라이버가 처리했다.
- Linaro의 Exynos850 hotplug 시리즈 v7 소개글(2026-09-30): "As a next steps it will be possible to enable idle states on top of it (if we get our hands on correct firmware)." Linaro도 지금 펌웨어로는 E850-96의 idle 상태를 켜지 못했다. 사용자 정리 문서는 [E85096-C2-CPUIDLE-REFERENCE-20261102.md](E85096-C2-CPUIDLE-REFERENCE-20261102.md).
- 하지 않은 시험: C2 진입 직전에 GIC-400 CPU 인터페이스(GICC_CTLR.Enable)를 끄기. 커널 GICv2 드라이버는 `CPU_PM_ENTER`에서 레지스터를 저장만 한다.
- 결론: C2는 펌웨어(BL31/APM) 문제일 가능성이 크다. 보드는 `boot-nokvm`(`cpuidle.off=1`)으로 되돌렸다. C2를 막은 부트 이미지(`~/e850-logs/boot-c2t`)는 `cpuidle.off=1` 없이도 부팅되지만 얻는 것이 없다(WFI만 쓰므로 같다).
- 덤으로 알게 된 것: 멈춘 상태에서도 워치독 데몬이 살아 있는 CPU에서 계속 신호를 보낼 수 있다. 그래서 리셋까지 몇 분이 걸리거나 전원을 껐다 켜야 할 수 있다. 다시 시험한다면 `s3c2410_wdt.tmr_atboot=1`(커널이 워치독을 부팅 때부터 켬)도 함께 넣어, 멈춰도 사람 손 없이 fastboot로 돌아오게 한다.

### 11.13 해결: CPU hotplug (`modules/e850-cpuhp`)

mainline에 Linaro의 Exynos850 hotplug 패치가 올라와 있다([PATCH v5 4/6] "soc: samsung: exynos-pmu: add Exynos850 CPU hotplug support", Alexey Klimov, 2026-06, E850-96에서 시험). vendor 5.10 `exynos-cpupm`과 비교하면 두 가지가 다르다.

1. `CLUSTERx_CPUy_INT_EN`(PMU 0x11860000 + 0x1044 + cluster×0x400 + core×0x80)의 **bit 3**을 끌 때 1, 켤 때 0으로 바꾼다. vendor의 `flexpmu_cal_cpu_s5e3830.h`에서는 바로 이 줄들이 주석으로 막혀 있다.
2. 켜기 쪽(INFORM 지우기, GRP2 wake-up 마스크 해제와 pending 지우기)을 **`CPU_ON` 전에** 부팅 CPU에서 한다(`CPUHP_BP_PREPARE_DYN`). vendor는 새 코어가 이미 돌기 시작한 뒤에(`CPUHP_AP_ARM_TWD_STARTING`) `cal_cpu_enable()`을 부른다. 그런데 코어는 그 전 단계를 넘지 못한다.

`modules/e850-cpuhp/e850-cpuhp.c`가 이 두 가지를 cpuhp 콜백으로 넣는다. 끄기는 `CPUHP_AP_ONLINE_DYN` teardown(꺼지는 코어에서 `CPU_OFF` 전)이고, 켜기는 `CPUHP_BP_PREPARE_DYN`이다. 레지스터는 `ioremap`으로 직접 쓴다.

```text
e850-cpuhp: cpu2 offline: inform 1 int_en fcc status 1 grp2_en 4
psci: CPU2 killed (polled 0 ms)
e850-cpuhp: cpu2 before on: inform 10001 int_en ffc status 0 grp2_en 4    ← 꺼진 동안 전원 0, BL31이 INFORM에 0x10000을 더함
CPU2: Booted secondary processor 0x0000000200 [0x410fd051]
```

시험(`/root/hp.sh 30 300`): CPU1~7 전부 끄고 켜기 30회 + 무작위 한 코어 토글 300회. 1분 19초 걸렸고 **실패 0**, `failed to come online` 0, 끝난 뒤 `online=0-7`.

설치: 보드의 `/lib/modules/$(uname -r)/e850-cpuhp.ko` + `/etc/modules-load.d/e850-cpuhp.conf`(부팅 때 자동 적재). `build-rootfs.sh`도 `ext-e850-cpuhp`가 빌드돼 있으면 같은 설정을 넣는다. 빌드:

```sh
docker run --rm -v ~/e850-a13-build:/work -v e850-kernel:/work/kernel -v e850-out:/work/out \
    -v "$PWD/modules/e850-cpuhp:/m:ro" e850-build-env:1 bash /m/build-module.sh
```

다음: `cpuidle.off=1`(11.12)도 같은 깨우기 경로를 쓴다. C2 idle에 들어가기 전과 나온 뒤에도 같은 PMU 쓰기가 필요할 것이다. mainline 시리즈도 idle은 "후속 작업"으로 남겨 두었다.

### 11.14 HDMI 텍스트 콘솔 색 (흰 글씨) (`modules/e850-fbpal`)

명령줄 `vt.default_*`로 기본 글자색(7번)을 흰색으로 정했는데도 화면 글자가 진한 파랑으로 나왔다. framebuffer 픽셀을 직접 읽어(`/root/fbhist.py`: fb0 mmap의 색 히스토그램) 원인 셋을 찾았다.

1. fb0의 `fix.visual`이 부팅 때 **MONO01(0)**이다. fbcon은 붙을 때 한 번만 컬러/흑백을 정하므로 흑백 모드가 된다. 흑백 모드에서는 ANSI 색을 무시하고 "보통 = 1번, 굵게 = 2번" 색으로 그린다(측정: 모든 색이 0xaa0000, 굵게는 0xaa00).
2. DPU의 `decon_setcolreg()`는 DECON이 꺼짐이나 hibernation이면 팔레트 쓰기를 버린다. 그래서 콘솔이 팔레트를 다시 넣어도(`ESC ] R`) 반영되지 않는다.
3. pseudo-palette는 VGA 순서(1 = 파랑, 4 = 빨강)다. ANSI 순서인 `default_red/grn/blu`를 그대로 넣으면 빨강과 파랑이 바뀐다.

`e850-fbpal.ko`는 32bpp RGB면 `fix.visual = TRUECOLOR`로 고치고, `default_*`를 VGA 순서로 바꿔 fb0의 pseudo-palette에 직접 쓴다. `e850-hdmi.service`가 LT8912B 설정 뒤 이 모듈을 올리고 fbcon을 다시 붙인다(`vtcon1/bind` 0 → 1).

결과(재부팅 후): 검정 배경(0x000000), 흰 글씨(0xffffff). `\033[31m/32m/34m`이 각각 0xaa0000, 0xaa00, 0xaa로 나와 색 순서도 맞다. 화면을 눈으로 보는 확인은 사용자에게 부탁한다.

**정정(11.15):** 위의 "결과"는 fb0 메모리만 잰 값이다. 그때 HDMI 출력은 꺼져 있었다. 사용자는 이 설정으로 흰 글씨를 본 적이 없다.

### 11.15 색을 바꾼 뒤 HDMI가 꺼져 있던 문제: `decon_release()` (2026-11-02 07시)

**증상.** 11.14 이후 HDMI에 아무것도 나오지 않았다. 서비스는 active였다. fb0 메모리에는 흰 글자가 있었다(`fbhist.py`). LT8912B 칩 ID도 정상이었다.

**원인(UART 로그로 확인).**

```
e850-fbpal: fb0 palette loaded
sh: decon_release → decon_disable → dsim_disable → panel power(0)   ← echo 0 > vtcon1/bind
Console: switching to colour frame buffer device 240x67             ← echo 1 > vtcon1/bind
decon_pan_display: decon0 state(5), UNBLANK missed
```

- vendor `decon_release()`는 open 수를 세지 않는다. **fb0가 닫힐 때마다** DECON, DSIM, 패널 전원을 끈다. fbcon unbind도 여기에 들어간다.
  - 그래서 `fb-hold.py`가 fd를 쥐고 있어도 rebind 순간 출력이 꺼졌다.
  - `fbhist.py`나 `ov5647-cap.py --live`처럼 fb0를 열었다 닫는 프로그램도 화면을 끈다.
  - 그동안 "마지막 fd를 닫으면 꺼진다"고 적었는데, 이 설명은 틀렸다.
- DECON이 꺼져 있을 때 fbcon이 한 pan은 버려진다(`UNBLANK missed`). DECON을 다시 켜도 누군가 pan을 하기 전에는 fb0가 화면에 나가지 않는다.
  - 측정: unblank만 했을 때 사용자 화면은 검정이었다.
  - 다른 프로세스에서 `FBIOPAN_DISPLAY`를 하자 흰 글자가 잠깐 보였다. 그 프로세스가 fd를 닫는 순간 다시 꺼졌다.

**고친 것.**

| 파일 | 변경 |
|---|---|
| `modules/e850-hdmi/apply.sh` | `decon_core.c`에 `decon_open()`(`.fb_open`)을 추가하고 open 수를 센다(`atomic_t decon_fb_users`). `decon_release()`는 마지막 사용자일 때만 끈다. fbcon도 `fb_open`/`fb_release`를 짝으로 부른다 |
| `tools/linux510/e850-hdmi.service` | fbpal 적재와 fbcon rebind를 `ExecStartPost`에서 `ExecStartPre`로 옮겼다(`fb-hold.py`가 fb0를 열기 전) |
| `tools/linux510/fb-hold.py` | unblank 직후 `FBIOPAN_DISPLAY`를 한다 |

`dpu.ko`는 initramfs 1단계 모듈이 아니다(`autoload.deny`). 서비스가 rootfs에서 올린다. 그래서 모듈만 다시 빌드했다(`kb34.log`). boot.img는 그대로다.

**결과(재부팅 후).**
- 부팅 때 fbcon unbind에서 DECON이 한 번 꺼진다. 그때 사용자가 fbcon 하나뿐이라 정상이다. 그 뒤 `fb-hold.py`가 다시 켠다.
- `fbhist.py`를 두 번 실행했다. 로그에는 `decon_release +`만 있고 `decon_disable`은 없다.
- **사용자가 HDMI 화면에서 흰 글씨를 눈으로 확인했다.**

**확인하지 않은 것.**
- `ov5647-cap.py --live`를 실행했다 끝낸 뒤의 화면
- HDMI 케이블을 다시 꽂았을 때
- DECON hibernation 뒤의 색
- LT8912B HPD(`0xc1` bit7): 화면이 나올 때도 `not connected`로 읽혔다. 이 레지스터 판정은 믿을 수 없다.

### 11.16 콘솔 색 근본 수정: dpu.ko에서 visual과 팔레트 (2026-11-03)

11.14의 `e850-fbpal`과 fbcon rebind를 없애려고 dpu 드라이버 두 곳을 고쳤다(`modules/e850-hdmi/apply.sh`).

| 위치 | 원래 동작 | 고친 것 |
|---|---|---|
| `decon_core.c` 창 생성 | `fix.visual`을 정하지 않는다(0 = MONO01). 값은 `decon_set_par()`에서만 정해지는데, 이 함수는 DECON이 꺼져 있으면 그냥 return한다. fbcon이 붙을 때 DECON은 꺼져 있다 | `fbinfo->fix.visual = FB_VISUAL_TRUECOLOR`(32bpp 고정) |
| `decon_dsi.c` `decon_setcolreg()` | DECON이 OFF이면 쓰기를 버린다. 매 호출마다 `printk("@@@@ ...")`를 한다 | OFF 검사와 printk, 쓰지 않게 된 `decon` 변수를 뺐다. pseudo-palette(`u32[16]`)는 메모리일 뿐이라 하드웨어를 건드리지 않는다 |

- `e850-hdmi.service`에서 fbpal 적재와 fbcon rebind 줄을 뺐다.
- 빌드는 `kb36.log`이고, 경고는 없다.
- 새 dpu.ko의 md5는 `4512e825…`다. 11.15판은 `/root/dpu.ko.opencnt`에 남겼다.

**결과(재부팅 후, 2026-11-03).**
- `Console: switching to colour frame buffer device 240x67`: 부팅 때부터 색 모드로 붙는다. 전에는 mono였다.
- `e850_fbpal`은 적재되지 않았다.
- 부팅 뒤 `decon_disable`이 없다. 전에는 rebind 때 한 번 꺼졌다.
- tty1에 `\033[31m RED \033[32m GREEN \033[34m BLUE \033[0m WHITE`를 쓰고 fb0 메모리를 쟀다. 결과는 0xaa0000, 0x00aa00, 0x0000aa, 0xffffff, 배경 0x000000이다. 색 순서도 fbcon이 맞게 바꾼다.

**결과(전원을 껐다 켠 뒤, 2026-11-06).**
- dmesg에서 `colour frame buffer device`가 찍혔다.
- `@@@@` 0건, `decon_disable` 0건이다.
- 사용자가 HDMI 화면에서 흰 글씨를 확인했다.

**확인하지 않은 것:** DECON hibernation 뒤의 색, 케이블을 다시 꽂았을 때, `ov5647-cap.py --live`를 끝낸 뒤. OV5647은 DVS로 교체되어 마지막 항목은 시험할 수 없다.

### 11.17 DVS(S5KRC1S) I2C 통신 확인 (2026-11-06, 재부팅 없음)

OV5647을 떼고 DVS S5KRC1S를 CN4에 연결했다. 22핀→15핀 Raspberry Pi FPC로 2-Lane 연결이고, 센서는 내부 클록을 쓴다. 드라이버를 만들기 전에 I2C만 확인했다. 참고 자료는 연결/포팅 가이드 슬라이드와 `modules/dvs/README.md`(Linux 7.2.8, 2026-10-07)다.

**버스(관찰).** CN4 13·14번(I2C4) = HSI2C2 = 핀 gpc1-4/5(`hsi2c2-bus`) = `138c0000.hsi2c` = Linux **`i2c-7`**. OV5647 때 쓰던 버스와 같다. DT의 `7-0036`(OV5647) 노드는 남아 있지만 fimc_is를 올리지 않아 바인딩된 드라이버는 없다.

**전원(관찰).** CAM_VDD_33(`vdd_ldo32`)은 `disabled`(3.3 V 설정, 사용자 0)였다. RESET 후보인 CN4 12번(MCLK2/GPC0[2], legacy GPIO 110)은 아무도 잡지 않은 입력 상태였다. 이 상태에서 `i2cdetect -y -r 7` 결과는 응답 없음이었다.

**보드에 한 것.** `modules/dvs-power` 모듈을 만들었다(`/root/dvs-power.ko`, 자동 적재하지 않음).
- insmod: `regulator_get(NULL,"vdd_ldo32")` enable → RSTN Low 10 ms → High
- rmmod: RSTN Low → LDO 끔
- MCLK는 내보내지 않는다.

**결과.**
```text
dvs-power: vdd_ldo32 on (3300000 uV), rstn gpio 110 high
i2cdetect -y -r 7   → 0x10, 0x36 응답 (전원 끄면 둘 다 사라짐)
0x10 reg16 0x0000..0x000f = 모두 0x00   (S5K 계열의 흔한 ID 위치지만 여기선 0)
0x10 0x303B: 0xAA 쓰기 → 0x2A, 0x55 쓰기 → 0x55   (10-07 기록과 같음), 끝나고 0x00으로 되돌림
0x10 0x3915 = 0x24, 0x3001.. = 0x27 0x20 0x00 0x03 0x07 0xd0 ...  (0이 아닌 값 다수)
0x36 reg16 0x300a, 0x0000.. 및 무주소 읽기 = 모두 0x00
```

**판단.**
- I2C 주소 0x10에서 레지스터를 쓰고 되읽을 수 있다. 제어 통신은 확인했다.
- **CHIP_ID는 확인하지 못했다.** S5KRC1S 레지스터 맵이 없고, 0x0000은 0이다.
- 0x36의 정체는 모른다. DVS 모듈 전원으로 켜지는 다른 장치로 보이지만 가설이다.
- RSTN 극성과 22핀 18번 ↔ 15핀 12번 연결은 직접 재지 않았다. RSTN을 Low로 둔 상태에서 응답하는지도 시험하지 않았다.
- rmmod 뒤 gpc0-2는 출력 Low(CON=1, DAT=0)로 남는다. 재부팅하면 원래 입력 상태로 돌아간다.

### 11.18 S5KRC1S Pablo CIS 드라이버: OV5647과 같은 ISP 우회 경로 (2026-11-06, 부팅 시험 전)

**매뉴얼에서 확인한 것.** 참고 자료는 `S5KRC1S_센서_user_manual.pdf`(사용자 제공, 해석본)다.
- Table 3: RSTN Low = Hardware Standby, RSTN High = Software Standby(I2C 가능). RESET은 **active-low**다.
- Table 4: `SEL_MODE_r` 0x3000[1:0]은 0=SW standby, 1=Monitor, 2=Stream. `MODE_SELECT_r` 0x0100[0]은 RO다. 11.17에서 0x3000을 읽은 값 0x00은 SW standby라는 뜻이다.
- CCI는 최대 400 kHz, MIPI CSI-2는 4-lane(레인당 최대 2.5 Gbps)이다. 960×720, RAW2와 32비트 압축 프레임 형식을 쓴다.
- **칩 ID 레지스터는 해석본에 없다.** OTP에 칩 ID가 있다는 설명만 있다.

**드라이버.** 커널 트리에 있던 S5KRC1S 브링업 드라이버를 `modules/s5krc1s-pablo`로 가져와 고쳤다. 바꾼 내용은 그 README의 표에 있다.
- 핵심 변경: RSTN을 GPG1[5]에서 GPC0[2]로 옮겼다. MCLK를 끄고 LDO32 3.3 V를 켠다. `front_sensor_id` = 42와 VISION 시나리오를 넣었다. probe에서 `__init`을 빼고 `module_i2c_driver`로 바꿨다.
- ISP 우회(bypass) 패치(`modules/ov5647-pablo/bypass.sh`)는 이미 fimc-is에 들어 있어 그대로 쓴다.
- setA, 0x3915 2-lane, 모드(RAW8 8192×127, 1600 Mbps/lane)는 바꾸지 않았다. 측정하지 않은 값이다.

**빌드와 보드 변경.**
- `apply.sh`로 커널 트리(볼륨 `e850-kernel`)의 보드 dts include를 OV5647에서 S5KRC1S로 바꿨다. dtbs를 다시 빌드했다.
- 부트 이미지는 `~/e850-logs/boot-dvs/boot.img`다. 저장소 빌드라 `kvm-arm.mode=none`은 없다(커널이 무시하는 인자). merged.dtb 차이는 카메라 노드뿐이다. 전원 순서의 GPIO가 gpc0 2를 가리키고, LDO32가 3.3 V이고, `front_sensor_id`가 0x2a인지 확인했다.
- 보드의 `/lib/modules/$(uname -r)/is-cis-s5krc1s.ko`를 새 판(md5 `2cd773b7…`)으로 바꿨다. 원본은 `/root/is-cis-s5krc1s.ko.orig`다. 지금 구워진 DT에는 이 센서 노드가 없어서 재부팅 전에는 영향이 없다.

**아직 하지 않은 것.** boot-dvs 굽기와 재부팅(사용자 허락 필요). 그다음 `modprobe fimc-is && modprobe is-cis-s5krc1s`, S_INPUT(0x10201)으로 SEL_MODE 응답을 확인하고, CSI 프레임 수신을 시험한다.

### 11.19 boot-dvs 부팅, 드라이버 동작, CSI 수신 없음 (2026-11-06)

**보드에 한 것.**
- `boot-dvs`를 구웠다. 처음에는 fastboot 대기 루프의 `sleep`이 막혀 fastboot 창을 놓쳤고, 워치독이 다시 리셋해 정상 부팅됐다. 두 번째에 구웠다.
- 부팅 정상. nginx, ssh, e850-hdmi, e850-wifi 모두 active다.
- `/proc/cmdline` 앞에는 `kvm-arm.mode=protected`가 붙는다. 이미지 밖에서 붙는 값이다.
- `/usr/local/bin/ov5647-cap.py`에 `--raw8` 옵션(SBGGR8, 8비트)을 더했다. 이전 판은 `/root/ov5647-cap.py.bak`이다.
- `modules/csis-peek`을 만들었다. CSIS 레지스터를 한 번 출력하고 `-EAGAIN`으로 적재를 끝내는 모듈이다. 이 커널에는 `/dev/mem`이 없다.

**관찰.** 실행한 명령은 `modprobe fimc-is && modprobe is-cis-s5krc1s`와 `ov5647-cap.py --raw8 -W 8192 -H 127`이다.
```text
cis_s5krc1s_probe: S5KRC1S bound to sensor instance 1, addr=0x10          (7-0010 → S5KRC1S)
[M42] pin_ctrl vdd_ldo32 → sen_rst low (pin 110) → sen_rst high → on_i2c
sensor_s5krc1s_cis_init: S5KRC1S 960x720 DVS, SEL_MODE(0x3000)=0x00
sensor mode(8192x127@30) = 0 (lane:2); 0x3915=0x06; CSI2_DI=0x2a ... 되읽기 모두 설정값
[CSI1] settle = 8, speed(1600 Mbps), 2 lane; [VC0] DMA format(2a) (8192, 127)
sensor_s5krc1s_cis_stream_on: stream ON → SEL_MODE=0x02
is_sensor_dtp: DTP detected        (300 ms 안에 frame start 없음) → VC0 프레임 0
```
- 스트리밍 중 I2C로 읽은 값: `0x30A0=0x01`(전역 설정이 들어감), `0x30A2=0x11`, `0x30A3=0x1f`, `0x3000=0x02`.
- CSIS 오류 인터럽트는 0건이다.
- CSIS1(0x14410000) 스트리밍 중 레지스터:
  - `CMN_CTRL=0x4901`, `PHY_CMN_CTRL=0x7`(클록 레인 + 데이터 2레인 켬)
  - `INT_SRC0/1=0`, `FRM_CNT_CH0=0`
  - **`PHY_STATUS=0xf1`을 10 ms 동안 계속 유지**: STOPSTATECLK=1, STOPSTATEDAT=0xF, ULPS 없음
- 스트리밍 중 `0x3915`를 0x26으로 바꿔도 결과가 같았다. 0x26은 MIPI_CK_MODE=10b(클록 항상 켬)이고, 가이드 `DVS_S5KRC1S_I2C_Initialization_Operation_Guide.md` 11절의 권장값이다.

**판단.**
- 센서 쪽은 I2C, 전원, RESET, 모드 전환까지 동작한다.
- CSIS는 클록 레인에서 HS를 한 번도 보지 못했다. 센서 MIPI 송신부가 HS 클록을 내지 않거나, 클록 레인이 SoC까지 오지 않는다.
- 가설 1(유력): PLL과 출력 클록이 설정되지 않았다.
  - setA는 "vendor 전체 초기화 표가 아님"이라고 스스로 적고 있다.
  - 매뉴얼 블록도에는 "PLL & Clock generator via External Clock"이 있다. RPi5 계획 문서의 데이터시트 추출본에는 EXTCLK 6–64 MHz, PLL reference 6–12 MHz가 있고, "PLL divider 표는 vendor table 필요"라고 되어 있다.
  - CN4 12번은 RSTN으로 쓰므로 SoC MCLK를 줄 수 없다. 모듈에 발진기가 있는지("Internal Clock")는 확인하지 않았다.
- 가설 2: 22→15 케이블의 클록 레인(8·9번) 배선 문제. 도통은 재지 않았다.
- STOPSTATEDAT=0xF는 리셋값과 같아서, 선이 끊겨 있어도 이렇게 보일 수 있다. 이 값만으로 둘을 구분할 수 없다.

**다음.**
- vendor init 시퀀스(PLL/출력 PLL, "Delta_10_2000FPS")를 확보한다.
- 모듈의 클록원(발진기 유무)을 확인하고, 케이블 8·9번 도통을 잰다.
- 그 전에 소프트웨어로 해 볼 수 있는 것은 4-lane/1-lane 설정(0x3915[1:0])과 Monitor 모드뿐이다. 클록 레인 상태를 바꿀 근거는 없다.

### 11.20 vendor setfile(Delta_10_2000FPS)로 MIPI 수신 시작, CSIS overflow 패닉 (2026-11-06)

**자료.** 라즈베리파이 5용 드라이버 패키지 `~/Documents/S5KRC1S_DVS_MIPI_Driver_RPi5_20260821.tar.gz`와 저장소 `philipdekim-OnD01/DVS.Decoder.RP5.01`을 참고했다.
- vendor setfile `Delta_10_2000FPS.txt`(83줄)가 들어 있다. setA에 없던 PLL P/M/S(0x3070–0x3073), PLL 분주(0x3079–0x307B), PLL_PD(0x307E), D-PHY PLL PMS(0x30A5–0x30A7), 클록 enable(0x3080/81/83)이 모두 있다.
- RPi5 저장소의 `sensor_registers_before_key.txt`에는 동작 중 레지스터 값이 있다. `0x3915=0x04`(4-lane), `0x3901=0x2a`, `0x3910=0x81`, `0x391A=0x7f`다.
- 저장소에는 `modules/s5krc1s-pablo/setfile/`로 넣었다.

**0x36의 정체.** setfile의 `0x3091=0x6C`(CCI_ADDRESS_CONTROL_2ND)를 7비트로 바꾸면 0x36이다. 11.17의 덤프에서도 0x3091은 0x6c였다. 0x36은 센서 자신의 두 번째 CCI 주소다.

**실패한 실험: 스트리밍 중에 setfile 쓰기.** setA로 스트림을 켠 뒤 standby로 내리고, Delta 전체와 0x3915=0x06 또는 0x26을 쓰고 다시 stream을 켰다. 쓰기는 들어갔다(`0x3080=0x3f`, `0x3915=0x26` 되읽음). 그래도 PHY_STATUS는 0xf1, 프레임은 0이었다.

**드라이버 변경(`modules/s5krc1s-pablo`).**
- setA Global을 Delta 전체로 바꿨다. RO인 0x0100은 뺐다.
- `cis_set_global_setting`에서 setfile을 쓰기 전에 500 ms 기다린다. RPi5 도구가 RSTN High 뒤 0.5 s를 기다리기 때문이다.
- set_two_lane은 `0x3915[1:0]`만 바꾼다. setfile 0x24에 2-lane 비트를 더해 0x26(클록 항상 켬)이 된다.

**결과(재부팅 후 첫 스트림, ssh `dmesg -w`로 Mac에 기록: `~/e850-logs/dvs-dmesg-w.log`).**
```text
sensor_s5krc1s_cis_set_global_setting: Delta_10_2000FPS written
sensor_s5krc1s_set_two_lane: 0x3915=0x26 (2-lane)
stream ON (0x3000=0x02)
[bypass] FS G13 ... fcount 1          ← CSIS frame start
[CSI1][VC0][F1] DMA on [3/1/0]
[WRN][CSIS1] start/end overlapped
is_sensor_dtp: DTP detected
Sensor Stream Off ...
Kernel panic - not syncing: CSIS error!! CSIS_ERR_OVERFLOW_VC     (ksoftirqd/0)
```
- **처음으로 CSIS가 MIPI 프레임을 받았다**(frame start, DMA on, start/end overlapped).
- 스트림을 끄는 시점에 VC overflow가 나고, `OVERFLOW_PANIC_ENABLE_CSIS`(v8_20_0 is-config.h)가 패닉을 냈다. `panic=0`이라 자동으로 재부팅되지 않는다.
  - 첫 번째(11:28 UTC)에는 워치독이 리셋하지 않아 사용자가 전원을 껐다 켰다.
  - 두 번째는 워치독이 리셋했고 보드가 fastboot에 들어갔다.
- `/proc/last_kmsg`에는 LK 로그만 있고 커널 로그는 없다.

**조치.** `apply.sh`에서 v8_20_0의 `OVERFLOW_PANIC_ENABLE_CSIS`를 `#undef`로 바꿨다. v8_10_0과 v8_1_0도 같은 방식으로 꺼 두었다. fimc-is.ko는 다시 빌드한다.

**조치 후 결과(같은 부팅, 재부팅 없음. 새 fimc-is.ko와 is-cis-s5krc1s.ko를 modprobe).**
- 캡처 1(`ov5647-cap.py --raw8 -W 8192 -H 127 -n 3 -b 4 -B 8 -t 3`): 패닉 없이 보드가 살아 있었다. **사용자 공간에 온 VC0 프레임은 0개다.**
  - dmesg: `[CSI1][VC0][F1] DMA on`이 한 번 나온 뒤 CSIS 프레임 카운터가 약 3초 동안 F38까지 올랐다. DMA done은 한 번도 오지 않았다.
  - stop 시점: `CSIS_ERR_VRESOL_MISMATCH`, `DMAFIFO_FULL`, `OVERFLOW_VC`, `start/end overlapped`.
  - 가설(측정 안 함): 센서의 timeout flush(0x3910 bit7)가 프레임을 127줄보다 짧게 끊어 CSIS가 세로 해상도 불일치로 본다. RPi5 기록에서는 프레임이 약 21줄이다.
- 캡처 2: `-t 6`으로 시작하고 1.3초 뒤 `i2ctransfer -f -y 7 w3@0x10 0x39 0x10 0x01`(timeout 끔)을 썼다.
  - 로그는 `[F1] DMA on`(2123.4 s)까지만 있다. 캡처 중에는 콘솔 printk를 꺼서 그 뒤 기록이 없다.
  - **약 2130 s부터 CPU0(sugov:0)과 CPU1(kworker/u16:3)에서 soft lockup이 났다.** ping, ssh, ACM 모두 응답이 없다. 워치독 데몬이 살아 있어 6분이 넘도록 리셋되지 않았다. 전원을 다시 넣어야 한다.
  - 원인이 timeout을 끈 쓰기인지 stop 경로(overflow 처리)인지는 확인하지 못했다. 패닉을 끈 뒤로는 overflow가 패닉 대신 반복되고 있을 수 있다.

### 11.21 프레임 주기를 1000 FPS로 낮추는 시도, 스트림 중 soft lockup 재발 (2026-11-06)

**사용자 요청.** 2000 FPS는 너무 빠르니 1000 FPS로 받는다.

**드라이버 변경.** 매뉴얼 본문에는 0x391x(OUTIF timeout) 정의가 없다. vendor setfile 주석에 따르면 `to_scnt0`(0x3911=0x7C)이 1 µs, `to_scnt1`(0x3912/13=0x03E7, 999)이 1 ms다. setA의 `to_scnt1`을 1999(0x07CF)로 두 배 늘렸다. 이 값이 프레임 주기를 정한다는 것은 **가설**이다. 측정은 아직 못 했다.
- 모듈은 `build-module.sh`(외부 모듈)로 빌드했다. md5 e296fa2f…다. 보드의 `/lib/modules/.../is-cis-s5krc1s.ko`를 바꿨다. 이전 모듈은 `/root/is-cis-s5krc1s.ko.2000fps`에 있다.
- 빌드 함정: 트리 안 단일 타깃(`make .../is-cis-s5krc1s.ko`)으로 빌드하면 안 된다.
  - PATH에 clang이 없으면 syncconfig가 실패하고 `include/generated/autoconf.h`를 지운다. clang을 PATH에 넣고 다시 돌리니 새로 생겼다.
  - clang이 있어도 modpost가 fimc-is 심볼을 찾지 못한다.

**결과.**
- 캡처 1(`-t 3`): 보드가 살아 있었다. 결과는 이전과 같다. F1에서 DMA on, F38에서 `OVERFLOW_VC`, `DMAFIFO_FULL`, `DMA_ABORT_DONE`, `VRESOL_MISMATCH`가 났다. `frame was done with error` 뒤 VC0 NDONE과 CANCEL이 나왔고 사용자 공간에 온 프레임은 0개다.
  - 2000 FPS에서도 3초에 F38이었다. 따라서 이 숫자는 센서 프레임 수가 아니라 드라이버 쪽 카운트로 보인다(가설). 실제 프레임 속도는 아직 모른다.
- 캡처 2(`-t 4`): 1.5초와 2.5초에 `csis-peek.ko`를 insmod해 FRM_CNT(0x500)를 두 번 읽으려 했다.
  - 로그는 stream ON(475.6 s)에서 끝났다.
  - 약 500 s부터 CPU0(sshd)에서 soft lockup이 났다. 다시 전원을 넣어야 했다.

**정리.** lockup은 두 번 났다. 두 번 다 스트림 도중 무언가를 한 캡처였다(I2C 쓰기, insmod). 아무것도 하지 않은 3초 캡처는 두 번 모두 살아 있었다. 원인은 아직 모른다.
- soft lockup 줄에 스택이 없다. 캡처 스크립트가 콘솔 printk를 끈 상태에서 멈췄기 때문이다.
- 그래서 `ov5647-cap.py`에 `--loud`(printk를 끄지 않음)를 추가했다. 다음 시험은 `--loud`로 해서 UART에서 멈춘 위치를 본다.

**IRQ 측정(재부팅 후 첫 캡처, `-t 3`, 스트림 도중에는 `/proc/interrupts`만 두 번 읽음. 하드웨어 접근 없음).**
- IRQ 141(GIC hwirq 174 = SPI 142, DT의 `is_sensor1@14410000` interrupt-name `csi`, CSIS1 메인 인터럽트)이 CPU0에서 **스트림 시작 후 1.2 s 안에 280,796번** 왔다. 1.2 s와 2.2 s 사이에는 늘지 않았다.
- IRQ 142(SPI 145, VC DMA)는 4번이다.
- 약 30만 번/s는 프레임 단위(2000 FPS)가 아니라 줄 단위(2000 FPS × 127줄 ≈ 25만)나 오류 인터럽트가 반복되는 규모다(가설).
- 보드는 살아 있었다. 앞의 두 lockup이 모두 CPU0에서 났으므로, 이 CSIS 인터럽트 폭주가 원인 후보다(확인 안 함).
- 결과는 이전과 같다. F48에서 오류 프레임, 사용자 공간 프레임 0개.

### 11.22 CSIS IRQ 폭주의 원인과 수정, DVS 이벤트 데이터가 메모리에 들어옴 (2026-11-06)

**자동 복구 준비(사용자 외출, 전원을 다시 넣지 않아도 되게).** 보드에 `/etc/sysctl.d/90-e850-autoreboot.conf`를 추가했다.
- 설정: `kernel.softlockup_panic=1`, `kernel.panic=5`. soft lockup → 패닉 → 5초 뒤 리셋.
- sysrq `c`로 시험했다. 패닉 후 자동 재부팅됐다. 그 부팅의 34.7 s에 eMMC(cqhci) `dma_unmap_sg`→`dcache_inval_poc` Oops가 나서 다시 패닉했고, LK fastboot(`Abnormal RST_STAT`)로 들어갔다. `fastboot reboot`로 정상 부팅했다.
  - 같은 Oops는 uart.log에 전에도 3번 있었다(이번 이전 2번). 원인은 조사하지 않았다.
- 어느 쪽이든 Mac에서 `fastboot reboot`로 복구할 수 있다.

**IRQ 원인 측정: `modules/csis-irqstat`.** kretprobe로 `csi_hw_g_irq_src()`가 해석한 irq_src만 센다. CSIS 레지스터는 건드리지 않는다.
- 3초 캡처 결과: ISR 238,427번 중 오류가 있는 것이 238,427번이다.
  - `err_id` bit 16(**VRESOL_MISMATCH**): 238,245번
  - bit 4(OVERFLOW): 8,266번
  - FS 39, FE 38
- 소스 원인: `is-hw-csi-v1_2.c`의 `csi_hw_g_irq_src()`가 VC별 오류 상태(`ERR_LOST_FS/FE` 0x6xx, `ERR_VRESOL` 0x624, `ERR_HRESOL`)를 읽기만 하고 되쓰지 않는다. 이 상태가 남아 있어 link IRQ가 계속 걸린 것으로 본다. v5_x도 같다. 보통 센서에서는 VRESOL 오류가 나지 않아 드러나지 않았다.

**수정: `modules/s5krc1s-pablo/patches/csis-v1_2-err-clear.patch`.** `apply.sh`가 적용한다. fimc-is.ko를 다시 빌드했다(d092d0de…). 보드의 이전 모듈은 `/root/fimc-is.ko.nopanic`에 있다.
1. 읽은 뒤 위 네 레지스터에 읽은 값을 되쓴다(clear).
2. VRESOL mismatch를 오류로 치지 않는다. DVS 프레임 길이는 이벤트에 따라 달라진다.

**결과 1 (패치만).**
- IRQ가 **28만 → 267번**으로 줄었다.
- VC0 버퍼 3개가 사용자 공간에 왔다. 모두 ERROR(flags 0x4040)였다.
- 모든 IRQ에 OVERFLOW(bit 4)가 있었다. dmesg에는 프레임마다 `OVERFLOW_VC`, `DMAFIFO_FULL`, `DMA_ABORT_DONE`, `start/end overlapped`가 나온다. 프레임 간격은 10~60 ms다.
- devfreq는 CAM 650000(최저), INT 100 MHz(최저), MIF 676 MHz였다. 로그에는 `dvfs table is NOT selected`가 있다.

**결과 2 (devfreq 최대: `cat max_freq > min_freq`, mif/int/cam).** 재부팅하면 원래대로 돌아간다.
- **OVERFLOW가 사라졌다.** 이제 오류는 bit 1(**CRC**) 277/277번이다.
- **frame 0은 ERROR 없이 왔다(flags 0x4000).** frame 1과 2는 ERROR였다. 모두 `bytesused 0`이다(bypass 경로가 채우지 않음).
- 프레임 파일은 Mac의 `~/e850-logs/dvs-frames-20261106/`에 있다.

**데이터 확인: `tools/host/dvsdecode.py`.** 32비트 big-endian으로 읽고 매뉴얼 Figure 49/50 형식으로 해석한다.
- frame 0: 첫 줄만 데이터다. 순서는 ref_ts(`08000003`) → sub_ts(`088002ad`) → COLUMN(`04200000`, 첫 열 플래그) → EVENT GROUP이다. 알 수 없는 워드는 0개, 이벤트는 21,181개, 열 89개다.
- frame 1/2: 각각 127줄과 39줄이 차 있다. 3~5%는 표에 없는 워드다.
  - 대부분 `0000532f`, `00005330`처럼 값이 늘어나는 type 000000 워드와 type 000101 워드다. IMU처럼 디코더가 아직 모르는 패킷으로 보인다(가설).
  - frame_end의 프레임 번호가 섞여 있는 곳이 있다. CRC 오류로 데이터가 깨졌는지는 아직 모른다.
- 열 주소 최대값이 1783이다. 매뉴얼(0~959)보다 크다. 원인은 확인하지 않았다.
- **센서의 DVS 이벤트가 MIPI → CSIS → DMA → 사용자 공간까지 처음으로 왔다.**

**남은 것.**
- 프레임마다 CRC 오류가 있다(레인 속도/settle, 2-lane). 이 때문에 ERROR 플래그가 붙는다.
- devfreq를 매번 손으로 올려야 한다.
- 디코더가 type 000000(값 있음)과 000101을 모른다.
- 1000 FPS(`to_scnt1`)의 효과를 아직 측정하지 못했다.
