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
