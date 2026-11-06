# E850-96 bring-up 현재 상태 (2026-11-02)

이 문서는 새 세션에서 바로 이어서 일할 수 있도록 지금 상태를 정리한 것이다. 경과와 측정 결과는 [E85096-LINUX510-BOOT.md](E85096-LINUX510-BOOT.md) 11장에, 대화 요약은 [SESSION-LOG-20261101-02.md](SESSION-LOG-20261101-02.md)에 있다.

## 1. 전체 상태

| 항목 | 상태 | 문서 |
|---|---|---|
| 8코어 SMP | 동작한다. DT의 CPU `reg`를 `cluster<<8 \| core`로 고쳤다 | 11.6~11.8 |
| 1시간 8코어 부하 | 통과. 오류 0, 최고 87℃ | 11.11 |
| CPU hotplug | `e850-cpuhp` 모듈로 해결. 30회 + 무작위 300회 시험에서 실패 0 | 11.13 |
| C2 cpuidle | **보류.** 코어가 C2에서 깨어나지 않는다. 펌웨어 문제로 본다. `cpuidle.off=1` 유지 | 11.12 |
| 카메라 OV5647 | 55~63 fps(VTS 수정, 캡처 중 printk 끔). **2026-11-06 사용자가 떼어 내고 DVS 센서로 교체했다** | 11.10 |
| DVS 센서 S5KRC1S | I2C 확인: `i2c-7`(HSI2C2) 주소 0x10, 0x303B 쓰기/읽기 OK. 전원은 `modules/dvs-power`(LDO32 + RSTN GPC0[2]). 매뉴얼에 칩 ID 레지스터 없음, 0x36도 응답(정체 모름). Pablo CIS 드라이버 `modules/s5krc1s-pablo`(OV5647과 같은 ISP 우회) 빌드, boot-dvs로 부팅. 드라이버로 전원·I2C·stream on까지 동작. Delta_10_2000FPS setfile로 **CSIS가 MIPI 프레임을 받는다**(F38/3 s). 하지만 DMA done이 없어 VC0 프레임은 사용자 공간에 0개, stop 시 VRESOL_MISMATCH·OVERFLOW. overflow 패닉은 껐다. 스트림 도중 I2C 쓰기나 insmod를 한 시험에서 soft lockup이 두 번 났다(전원 재투입 필요). 1000 FPS 시도(`to_scnt1`=1999)는 아직 효과를 측정하지 못했다. IRQ 폭주 원인은 CSIS v1.2가 VRESOL 오류 상태를 지우지 않은 것이었다(패치로 수정). devfreq를 최대로 하면 **DVS 이벤트가 사용자 공간에 온다**(`tools/host/dvsdecode.py`로 AER 패킷 확인). 남은 문제: CRC 오류(1499 Mbps 수신 설정에서 줄지만 남음), 매뉴얼에 없는 패킷 type 000000/000101(손상인지 미확정). 캡처는 `dvscap.sh`. 타임스탬프로 잰 실제 프레임 간격은 494 µs(약 2024 fps)라서 1000 FPS 설정은 효과가 없다. 오류 10프레임이면 CSIS를 끄는 vendor 동작을 `fimc_is.e850_csi_err_stop=0`으로 풀었고, **`ov5647-cap.py --live --raw8`로 HDMI에 이벤트 영상(ON 초록/OFF 빨강)을 계속 띄운다**(`is_cis_s5krc1s.flush_us=250`이면 초당 약 11번 갱신, 11.25. to_scnt1은 프레임 주기가 아니라 MIPI flush 타임아웃이라 999로 되돌림) 32줄 모드(mode1)는 System MMU fault 패닉으로 보류(11.26). 디코딩·렌더를 C(`libdvsview.so`)로 옮겨 HDMI **30 fps**, 디코딩 5.2 ms/MB(numpy의 약 45배). 하지만 버퍼에 센서 시간의 약 14%만 들어온다(CRC 시 DMA abort로 추정, 11.27) | 11.17~11.27 |
| HDMI 콘솔 색 | 흰 글씨. dpu.ko 패치로 근본 수정했다(`e850-fbpal`과 rebind는 쓰지 않음). 전원을 껐다 켠 뒤에도 사용자가 화면에서 확인했다 | 11.14~11.16 |
| HDMI 출력 꺼짐 | 해결. `decon_release()`가 fb0를 닫을 때마다 DECON을 끄던 것을 open 수 세기로 고쳤다(dpu.ko 재빌드) | 11.15 |
| 네트워크 | Wi-Fi 192.168.0.236(ssh), USB NCM 192.168.7.1 | 11.5 |
| 서비스 | nginx(80), ssh, e850-hdmi, e850-wifi, systemd-timesyncd, 워치독 데몬 | |
| 사용자 계획 | MySQL + Apache로 출입 기록, YOLO nano 영상 처리 | |

## 2. HDMI / DECON / fbcon 문제

### 2.1 확인된 사실 (소스와 측정으로 확인함)

- **fb visual:**
  - DPU fb(`/dev/fb0`, 32 bpp, RGB 8:8:8)는 부팅 직후 `fix.visual = FB_VISUAL_MONO01`이다.
  - vendor dpu 드라이버에서 `fix.visual`을 바꾸는 곳은 `decon_set_par()`뿐이다.
  - `decon_set_par()`는 DECON 상태가 OFF나 INIT이면 visual을 설정하기 전에 return한다.
  - fbcon이 붙는 시점에 DECON은 아직 켜지지 않았으므로, fbcon은 흑백(MONO) 모드로 붙는다.
- **팔레트:** `decon_setcolreg()`는 DECON이 OFF나 HIBER일 때 쓰기를 버리고 성공을 돌려준다. 그래서 `vt.default_*`나 `ESC ] P`로 팔레트를 바꿔도 반영되지 않는다.
- **색 순서:** fbcon의 pseudo-palette 인덱스는 VGA 순서(1=파랑, 4=빨강)이고, `default_red/grn/blu` 배열은 ANSI 순서(1=빨강, 4=파랑)다. 변환하지 않고 넣으면 빨강과 파랑이 바뀐다.
- **`decon_release()`(11.15):** vendor 드라이버는 fb0가 닫힐 때마다 DECON, DSIM, 패널을 껐다. fbcon unbind도 포함이다. DECON이 꺼져 있을 때 한 pan은 버려진다(`UNBLANK missed`). 그래서 11.14 이후 HDMI 출력이 꺼져 있었다.
- **현재 해결책(11.16, `modules/e850-hdmi/apply.sh`의 dpu.ko 패치):**
  1. 창을 만들 때 `fix.visual = TRUECOLOR`로 설정한다. 그래서 fbcon이 부팅 때부터 색 모드로 붙는다.
  2. `decon_setcolreg()`는 DECON 상태와 관계없이 pseudo-palette에 저장한다. 그래서 fbcon이 `vt.default_*`를 넣는다(VGA/ANSI 변환은 fbcon이 한다). vendor 디버그 `printk("@@@@")`도 뺐다.
  3. `decon_release()`는 마지막 사용자가 닫을 때만 DECON을 끈다(11.15).
  4. `fb-hold.py`가 unblank하고 pan을 한 뒤 fd를 계속 쥐고 있다(11.15).
  - `e850-fbpal`과 fbcon rebind는 서비스에서 뺐다. 모듈 소스(`modules/e850-fbpal`)와 보드의 `.ko`는 남겨 두었다.
- **측정(`tools/board/fbhist.py`, fb0 메모리의 픽셀 히스토그램. HDMI 출력이 아니다):**
  - 배경 0x000000, 글자 0xffffff
  - `\033[31m` → 0xaa0000, `\033[32m` → 0x00aa00, `\033[34m` → 0x0000aa
  - 재부팅한 뒤에도 같다.
- **HDMI 화면:** 2026-11-02 07시(11.15), 재부팅 뒤 사용자가 흰 글씨를 눈으로 확인했다. 그 전(11.14)에는 출력이 꺼져 있었다. 11.16 모듈로 바꾼 뒤 전원을 껐다 켰을 때도(2026-11-06) 확인했다.
- **fb0 열고 닫기:** `fbhist.py`를 두 번 실행해도 `decon_disable`이 찍히지 않는다.

### 2.2 가설 (아직 확인하지 않음)

- DECON이 HIBER(화면 절전)에 들어갔다 나와도 색이 유지될 것으로 본다. `decon_set_par()`는 32bpp에서 visual을 TRUECOLOR로 설정하고, 팔레트는 메모리에만 있다. 측정은 하지 않았다.
- HDMI 케이블을 다시 꽂거나, `ov5647-cap.py --live`가 fb를 직접 쓰다가 끝난 뒤에도 화면과 색이 유지되는지는 모른다.
- LT8912B HPD 레지스터(`0xc1` bit7)는 화면이 나올 때도 `not connected`로 읽혔다. 판정에 쓰지 않는다.

### 2.3 실패한 실험

| 시도 | 결과 | 원인 |
|---|---|---|
| 부트 인자 `vt.default_red/grn/blu`만 설정 | 글자가 그대로 파랑 | fb가 MONO01이라 fbcon이 팔레트를 쓰지 않음 |
| `ESC ] R` / `ESC ] P`로 팔레트 재설정 | 변화 없음 | `decon_setcolreg()`가 OFF/HIBER에서 쓰기를 버림 |
| `e850-fbpal` 첫 버전(visual은 두고 팔레트만 채움) | 적재 실패 `-ENODEV` | visual 검사에 걸림. TRUECOLOR를 강제하는 것으로 고침 |
| `e850-fbpal` 둘째 버전(ANSI 순서 그대로 채움) | 빨강과 파랑이 바뀜 | VGA/ANSI 인덱스 차이. `vga_to_ansi[]` 표로 고침 |
| fbcon rebind를 `fb-hold.py` 뒤에 실행(11.14 서비스) | HDMI 출력 꺼짐. fb0 메모리만 정상이라 놓침 | rebind의 `decon_release()`가 DECON을 끔 |
| rebind 뒤 `fb0/blank`로 unblank만 함 | 검은 화면 | pan이 없어 fb0가 화면에 나가지 않음 |
| 다른 프로세스에서 pan | 흰 글씨가 잠깐 보였다가 사라짐 | 그 프로세스가 fd를 닫을 때 다시 꺼짐 |

### 2.4 보드에 실제로 바꾼 것 (이번 세션)

| 경로 | 변경 |
|---|---|
| `boot` 파티션 | **2026-11-06 오후부터 `~/e850-logs/boot-dvs2/boot.img`**(boot-dvs + DT mode1 8192×32, 11.26). 그 전 판은 `boot-dvs`, 복구용 정상 이미지는 `boot-nokvm` |
| `/lib/modules/.../is-cis-s5krc1s.ko` | 2026-11-06 모드별 줄 수(127/32) 판(01dd3c98…, 11.26). 이전: `/root/is-cis-s5krc1s.ko.mode0only` |
| `/lib/modules/$(uname -r)/e850-cpuhp.ko` | 추가. `/etc/modules-load.d/e850-cpuhp.conf`로 자동 적재 |
| `/lib/modules/$(uname -r)/e850-fbpal.ko` | 추가. 11.16부터는 올리지 않는다(파일만 남음) |
| `/lib/modules/$(uname -r)/is-cis-ov5647.ko` | VTS 수정판으로 교체. 원본은 `/root/is-cis-ov5647.ko.orig` |
| `/etc/systemd/system/e850-hdmi.service` | fbpal과 fbcon rebind를 뺌(11.16). 저장소와 같은 파일(md5 `d92fce11…`). 이전 판은 `/root/e850-hdmi.service.fbpal`(11.15), `/root/e850-hdmi.service.bak`(11.14) |
| `/usr/local/bin/fb-hold.py` | unblank 뒤 pan 추가(11.15). 이전 판은 `/root/fb-hold.py.bak` |
| `/lib/modules/$(uname -r)/dpu.ko` | 11.16 패치판(md5 `4512e825…`). 11.15판은 `/root/dpu.ko.opencnt`, 원본은 `/root/dpu.ko.orig` |
| `/home/philip/.ssh/authorized_keys` | 사용자가 Mac 키를 등록(`ssh-copy-id`). `ssh philip@192.168.0.236` 키 접속 가능. sudo는 비밀번호 필요 |
| `/usr/local/bin/ov5647-cap.py` | 캡처 중 printk 끄기, u32 LUT. 저장소와 md5 같음 |
| `/lib/modules/$(uname -r)/is-cis-s5krc1s.ko` | 11.18 판으로 교체. 원본은 `/root/is-cis-s5krc1s.ko.orig` |
| `/usr/local/bin/ov5647-cap.py` | `--raw8` 추가(11.19). 이전 판 `/root/ov5647-cap.py.bak` |
| `/root/csis-peek.ko`, `/root/dvsreg.sh` | CSIS 레지스터 읽기 모듈, 스트리밍 중 센서 레지스터 스냅샷 |
| `/lib/modules/.../is-cis-s5krc1s.ko` | 2026-11-06 `to_scnt1`=1999로 바꾼 빌드(e296fa2f…, 11.21). 이전 모듈: `/root/is-cis-s5krc1s.ko.2000fps` |
| `/lib/modules/.../fimc-is.ko` | 2026-11-06 CSIS 오류 clear 패치 + speed/settle 파라미터(7f47a2a8…, 11.22~11.23). 이전: `/root/fimc-is.ko.nopanic` |
| `/lib/modules/.../fimc-is.ko` | 2026-11-06 `e850_csi_err_stop` 파라미터 추가(a2149bb7…, 11.24). 이전: `/root/fimc-is.ko.prev-11.23` |
| `/usr/local/bin/ov5647-cap.py` | `--live --raw8`(DvsLive), SIGTERM으로 정상 종료(11.24), 버퍼 모아 디코딩(11.25), libdvsview.so 호출 + `--fps`(11.27). 저장소와 같은 파일 |
| `/usr/local/lib/libdvsview.so` | 추가(11.27). `tools/linux510/dvsview.c` 빌드 |
| `/lib/modules/.../is-cis-s5krc1s.ko` | 2026-11-06 to_scnt1 999 복원 + `packets`/`flush_us` param(54365f52…, 11.25). 이전: `/root/is-cis-s5krc1s.ko.1999` |
| `/etc/sysctl.d/90-e850-autoreboot.conf` | 추가(11.22). `softlockup_panic=1`, `panic=5` |
| `/root/csis-irqstat.ko` | 추가(11.22). kretprobe IRQ 계수기, 캡처 전에 insmod하고 끝나면 rmmod |
| `/usr/local/bin/dvscap.sh` | 추가(11.23). devfreq 최대 + 캡처 + IRQ 계수 |
| `/root/dvs-power.ko` | 추가(11.17). 수동 insmod로만 DVS 전원을 켬. 지금은 내려 둠 |
| `/root/` | 시험용 스크립트 `ovreg.py`, `hp.sh`, `fbhist.py`, `stress1h.sh`, `.ko` 사본 |
| 계정 / Wi-Fi | `philip` 계정. Wi-Fi는 PSK 해시만 보드에 있음(저장소에는 없음) |

## 3. 다음에 할 실험 3개

1. **HDMI 색 유지 시험 (위험 낮음, 재부팅 없음)**
   - `restart e850-hdmi`와 fb0 열고 닫기는 11.15에서 확인했다. 나머지가 남았다.
   - 할 일: `fbhist.py`로 색을 측정하면서 다음 상황을 차례로 만든다.
     - `systemctl restart e850-hdmi`
     - HDMI 케이블 뽑았다 꽂기
     - 콘솔 blank 시간이 지난 뒤(DECON HIBER) 깨우기
     - `ov5647-cap.py --live`를 실행했다 끝내기
   - 판정: 매번 배경 0x000000, 글자 0xffffff인지 본다.
   - 2.2의 가설 1·2를 확인하는 실험이다.
2. **dpu 드라이버 근본 수정: 끝남(11.15, 11.16).**
3. **YOLO nano CPU 추론 벤치마크 (보드 변경은 패키지 설치뿐)**
   - 할 일: ncnn 또는 ONNX Runtime으로 YOLOv8n/YOLO11n(모델은 사용자 확인 필요)을 8코어에서 돌린다. 320과 640 입력에서 fps와 온도를 잰다.
   - 이 결과로 카메라 → 추론 → HDMI 파이프라인을 어떻게 짤지 정한다.

선택 실험: C2 idle에서 GICC_CTLR를 끄는 시험. 보드가 멈출 수 있어서 사용자가 있을 때만 한다.

## 4. 사용자 결정 대기

- Apache 포트: nginx를 끄고 Apache를 80번에 올릴지, Apache를 8080에서 따로 띄울지
- YOLO 모델 종류와 입력 크기
