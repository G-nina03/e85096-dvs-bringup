# E850-96 Mali-G52: Debian GPU 가속 조사 및 Claude 실행 지침

작성: 2026-11-02 (KST)
상태: 조사 완료 / 사용자 보드에서의 실측 검증 대기
범위: Exynos850 E850-96, Debian 12 arm64, 벤더 Linux 5.10.157 및 메인라인 비교

## 0. 핵심 결론

- “Mali-G52는 Android 바이너리만 있어 Debian에서 무조건 llvmpipe를 써야 한다”는 진술은 정확하지 않다.
- Mesa의 오픈소스 Panfrost 드라이버는 Mali-G52 (Bifrost)를 지원하고 OpenGL ES 3.1을 제공한다.
- 2026-08-26 Linaro 개발자가 Exynos850/E850-96 계열 실제 테스트에서 `Mali-G52 MC1 (Panfrost)`, `OpenGL ES 3.1 Mesa 26.2.1`을 보고했다.
- 2026-09-09 Exynos850 GPU DT 노드/compatible을 추가하는 Linux 패치 v4가 게시됐고, 2026-09-15 maintainer가 적용을 확인했다. 단, GPU 노드는 기본 disabled이며 당시 기능은 제한적이다.
- GPU를 이용한 오프스크린 EGL/GBM 렌더링과 모니터 HDMI/GUI 출력은 별도 문제다. 같은 공개 테스트에서 GPU는 살아 있었지만 DPU가 활성화되지 않아 프레임버퍼 출력이 없었다.
- 현 저장소의 작업은 벤더 5.10.157 + Debian 12이다. 메인라인에서 검증된 GPU DT 패치를 이 벤더 커널에 그대로 복사하면 동작한다는 보장은 없다. 커널 드라이버 ABI, GPU 노드, clock/power/reset, DRM 드라이버, 메모리/IOMMU 등을 각각 확인해야 한다.
- 이 문서는 기존 Linux 부팅/카메라 작업과 충돌하지 않도록 진단만 우선한다. 커널/DTB 덮어쓰기, eMMC 플래시, GPU 강제 전원 인가를 권장하지 않는다.

## 1. 실제 증거와 제한

### Mesa 공식 지원

Mesa 공식 Panfrost 페이지는 G52의 OpenGL ES 3.1 / OpenGL 3.1을 지원한다고 명시한다. Panfrost 자체는 화면 출력 컨트롤러가 아니며, 디스플레이에는 별도 DPU/DRM(KMS) 경로가 필요하다. OpenCL과 하드웨어 비디오 코덱 가속은 Panfrost OpenGL 지원과 별개다.

- https://docs.mesa3d.org/drivers/panfrost.html
- https://wiki.debian.org/PanfrostLima
- Debian 12 arm64 패키지 안에 `panfrost_dri.so` 존재: https://packages.debian.org/bookworm/arm64/libgl1-mesa-dri/filelist
- Debian 12 arm64 `mesa-utils`에 `eglinfo` 존재: https://packages.debian.org/bookworm/arm64/mesa-utils/filelist

### Exynos850 실기기 공개 로그

2026-08-26 Linaro Alexey Klimov 답장: https://lists.openwall.net/linux-kernel/2026/08/26/1847

핵심 발췌:

```text
GBM platform:
EGL vendor string: Mesa Project
OpenGL ES profile renderer: Mali-G52 MC1 (Panfrost)
OpenGL ES profile version: OpenGL ES 3.1 Mesa 26.2.1-arch1.1
ls: cannot access '/dev/fb*': No such file or directory
```

이는 공개된 보드의 GBM/EGL GPU 가속 성공 기록이며, 현재 사용자 Debian 12/5.10 조합 또는 HDMI 데스크톱 가속의 성공 기록은 아니다.

### Exynos850 지원 패치

- 패치 v4 (2026-09-09): https://lists.openwall.net/linux-kernel/2026/09/09/2727
- 적용 확인 (2026-09-15): https://lkml.iu.edu/2609.1/17939.html
- DT binding commit: https://git.kernel.org/krzk/linux/c/5a881e2dfce03d6426d81beb35e68089a02aad89
- Exynos850 GPU DT node commit: https://git.kernel.org/krzk/linux/c/3d7bf30480f3a34fe530213da9e5bd0aa406929d

주의: 메인라인 적용은 Debian 배포 커널/현재 벤더 5.10에 해당 패치가 이미 포함되었다는 뜻이 아니다. upstream GPU node가 disabled인 상태라면 E850-96 보드 DTS에서 별도 검증 후 활성화해야 한다.

## 2. 현 저장소의 중요한 전제

- [벤더 5.10 부팅 실험 기록](E85096-LINUX510-BOOT.md): 5.10.157-00010-g3b6b9ca51034-dirty / Android 10 LK 유지 / Debian 12 arm64 rootfs.
- 같은 문서의 `tools/linux510/autoload.deny` 설명은 GPU 모듈을 자동 적재에서 제외했다고 명시한다. 실제 설치 버전의 initramfs 내 목록과 현재 `lsmod`를 다시 확인해야 한다.
- 부팅 기록에는 UART, cpuidle, USB gadget, boot.img 및 userdata 쓰기 관련 이슈가 있어 GPU 수정을 위해 부팅 체인을 흔들면 리스크가 크다.
- [Android → Linux 이전 가이드](E85096-ANDROID-TO-LINUX.md)는 서로 다른 커널 버전의 모듈은 그대로 호환되지 않는다고 명시한다.

## 3. Claude가 먼저 수행할 비파괴 진단

아래 명령은 MacBook 호스트가 아닌 E850-96 Debian 터미널/SSH에서 실행한다. 출력에 개인 계정, 토큰 등 비밀정보가 섞이면 공개 이슈에 업로드하기 전에 마스킹한다.

```bash
cat /etc/os-release
uname -a
dpkg --print-architecture

# DRM 렌더 노드가 있는가? /dev/mali0만 있는 경우 독점 벤더 스택일 수 있음
ls -l /dev/dri /dev/dri/* /dev/mali0 2>&1 || true
ls -l /sys/class/drm 2>&1 || true

# 바인딩된 GPU 드라이버와 probe 결과
lsmod | grep -Ei 'panfrost|mali|drm|gpu' || true
sudo dmesg | grep -Ei 'panfrost|mali|gpu|drm|iommu|regulator|devfreq' | tail -n 120

# 현재 커널에 드라이버가 빌드되었는가?
zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_DRM_PANFROST|CONFIG_DRM=|CONFIG_MALI|CONFIG_DEVFREQ' || true
grep -E 'CONFIG_DRM_PANFROST|CONFIG_DRM=|CONFIG_MALI|CONFIG_DEVFREQ' "/boot/config-$(uname -r)" 2>/dev/null || true
find "/lib/modules/$(uname -r)" -iname '*panfrost*' -o -iname '*mali*' 2>/dev/null
modinfo panfrost 2>&1 | head -n 25 || true

# 배포판 Mesa 드라이버 존재 여부
dpkg -l libgl1-mesa-dri libegl1 libgles2 libgbm1 mesa-utils 2>/dev/null || true
ls -l /usr/lib/aarch64-linux-gnu/dri/panfrost_dri.so 2>&1 || true
command -v eglinfo || true

# 현재 DT의 GPU 노드 검색(존재/상태 확인용)
find /proc/device-tree -iname '*gpu*' 2>/dev/null | head -n 35 || true
```

확인할 핵심: `mali_kbase`나 `/dev/mali0`만 존재해도 Mesa Panfrost용 DRM render node가 반드시 있는 것은 아니다. 커널에 무엇이 올라왔는지와 Mesa가 무엇을 사용할 수 있는지 분리해서 판정한다.

## 4. 설치 가능한 사용자 공간 라이브러리

Debian arm64에서 패키지 유무를 확인한 다음 설치한다. 이 단계는 라이브러리 설치이며 GPU 커널 드라이버나 DPU를 만들어 주지 않는다.

```bash
sudo apt update
sudo apt install -y libgl1-mesa-dri libegl1 libgles2 libgbm1 mesa-utils
dpkg -L libgl1-mesa-dri | grep panfrost
eglinfo -B
```

헤드리스/SSH 환경에서는 `eglinfo -B`의 `GBM platform` 또는 `surfaceless platform` 결과를 읽는다. `glxinfo -B`(X11) 한 줄의 llvmpipe만으로 GPU 전체가 사용 불가능하다고 판정하지 않는다. 반대로 `eglinfo`에 Mesa라는 문자열만 나오는 것도 GPU 가속 성공 증거가 아니다. Renderer에 명확하게 `Mali-G52 MC1 (Panfrost)`가 나오는지 확인한다.

## 5. 결과에 따른 분기

| 진단 결과 | 해석 | 다음 작업 |
| --- | --- | --- |
| `/dev/dri/renderD*` 존재, `eglinfo -B`에서 Mali-G52 (Panfrost) | GPU 사용자 공간 가속 성공 | GBM/EGL 최소 그리기 테스트, GUI/DPU는 별도 확인 |
| `panfrost_dri.so`만 존재, `/dev/dri/renderD*` 없음 | Mesa는 준비됐으나 GPU DRM 경로가 없음 | 커널 config / GPU DT / clock-power / probe 확인 |
| `/dev/mali0` 또는 `mali_kbase`만 있음 | Android/벤더 Mali 스택 가능성 | Panfrost와 동일시 금지. 충돌 없이 별도 커널 실험 계획 수립 |
| `panfrost` 로드되지만 probe 실패 | DT, regulator, clock, power-domain, IOMMU, DVFS 문제 가능 | 정확한 dmesg 오류부터 해결 |
| GPU EGL 성공, HDMI/데스크톱 llvmpipe 또는 화면 없음 | DPU/DRM KMS 및 X11/Wayland 경로가 별도 병목일 수 있음 | DPU 디바이스/DRM connector/planes 경로 검증 |

참고: DRM KMS만 쓰는 최신 시스템은 `/dev/fb0`가 없을 수 있으므로 그 파일의 부재만으로 DPU 실패를 단정하지 않는다. 공개 사례에서는 그 밖의 로그와 함께 DPU 미활성화를 지적했다.

## 6. 커널 개발 원칙: 5.10과 메인라인 분리

### A안 — 현재 5.10 + Debian 12에서 진단 및 최소 변경

1. 현재 `/proc/config.gz`, `/lib/modules/`, `lsmod`, `/proc/device-tree` 확인.
2. `CONFIG_DRM_PANFROST`가 있는지 확인한다. 없거나 API/DT compatible이 맞지 않으면, Mesa만 설치하여 해결되지 않는다.
3. 벤더 Mali 드라이버와 Panfrost가 같은 GPU를 동시에 소유하지 않도록 검토한다.
4. `autoload.deny` 내 GPU 항목은 의도된 부팅 안정화 설정이므로 원인 확인 전에 제거하지 않는다.
5. 커널 소스와 현재 DT/clock/regulator/GPU 연결을 읽기 전용으로 비교한다.

### B안 — 별도 실험 이미지에서 메인라인 Panfrost 재현

1. 2026-09 커밋이 포함된 커널 트리와 Exynos850 DT node, E850-96 board DT의 GPU status 등을 점검한다.
2. `CONFIG_DRM_PANFROST`, DRM render node, clocks/power domain/regulator 및 iommu 매핑을 검증한다.
3. 벤더 5.10에 무작정 DTS 코드만 복사하지 말고 패치 의존성과 드라이버 인터페이스를 검토한다.
4. 안정적인 5.10 boot/Android 복구 이미지를 보존하고 새 커널/DTB 실험은 복구 가능한 방법으로만 진행한다.

예시로 보이는 `&gpu { status = "okay"; };`는 해당 SoC의 올바른 GPU node와 전원/클럭 구성이 있을 때에만 의미가 있다. 현재 보드에 안전하다는 판단 없이 직접 적용하지 말 것.

## 7. DVS 프로젝트에서의 GPU 활용 우선순위

1. DVS 센서 I2C/CSI-2/DMA 수신 안정화는 GPU 없이 진행 가능. GPU 드라이버 작업이 카메라 부팅/복구 작업을 방해해서는 안 된다.
2. GPU가 GBM/EGL로 동작하면 off-screen DVS 이벤트 누적 이미지/타임서피스, OpenGL ES 시각화 PoC를 먼저 만든다.
3. 이후 DPU가 준비되면 화면 표시/전체 Debian 데스크톱 가속으로 확장한다.
4. Panfrost OpenGL ES 성공은 OpenCL, NPU, 영상 디코딩, 모든 AI 모델 추론 가속을 뜻하지 않는다.

## 8. Claude에게 요청할 실행·보고 형식

아직 사용자 보드에서 GPU 가속이 성공했다고 판단하지 말 것.

- FACT: 커널 릴리스, GPU kernel module, /dev/dri/renderD*, 실제 EGL renderer, DPU/DRM 상태.
- CAUSE: llvmpipe가 나오는 정확한 원인을 kernel/DT/Mesa/permissions/display별로 분리.
- EVIDENCE: `uname -a`, `lsmod`, `dmesg`, `eglinfo -B`, `/proc/device-tree` 출력(비밀정보 마스킹).
- NEXT: 되돌릴 수 있는 최소 조치 1개, 성공 기준과 실패 시 복원 절차.
- PASS-A (GPU): `eglinfo -B`가 실제 Mali-G52 (Panfrost) 렌더러를 보고 + 가능하면 GPU 사용 드로 테스트 성공.
- PASS-B (display): DRM connector 및 실제 Wayland/X11 환경에서 GPU 하드웨어 렌더링 확인. PASS-A와 독립 판정.

## 9. 참고 저장소 내 링크

- [Linux 5.10 부팅/개발 기록](E85096-LINUX510-BOOT.md)
- [Android → Linux 전환](E85096-ANDROID-TO-LINUX.md)
- [E850-96 프로젝트 README](../README.md)

작성 목적: Claude의 현재 커널/Debian 개발과 병행 가능한, 출처와 검증 기준이 분명한 GPU 문제 해결 작업 지시.
