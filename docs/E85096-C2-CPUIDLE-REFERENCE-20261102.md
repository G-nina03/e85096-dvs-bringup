# E850-96 / Exynos850 C2 cpuidle 장애 참고 메모 (2026-11-02)

> **문서 성격: 참고자료(Reference only).** Claude Code 또는 다른 개발자에게 새로운 실험·수정·플래시를 지시하는 문서가 아니다. 관찰 결과, 공개 패치 근거, 아직 증명하지 못한 가설을 분리하여 후속 분석 때 참고하기 위해 저장한다. **이 문서의 추가만으로 보드 상태를 변경하지 않는다.**

## 1. 배경 및 핵심 개념

E850-96 보드는 Samsung Exynos850 (Cortex-A55 8코어) 기반이다. Linux cpuidle의 C-state 이름은 플랫폼별 표기이며, 이 보드의 **C2는 더 깊은 CPU idle 경로**를 뜻한다. WFI는 얕은 idle의 대표적 방법이다. C2가 정확히 어떤 전원 도메인을 끄는지는 해당 보드의 DT, PSCI 상태값, 펌웨어, PMU 레지스터로 확인해야 한다.

- `cpuidle.off=1`: 커널 cpuidle 기능 자체를 비활성화한다.
- C2의 `wakeup-latency`를 `4026531840 µs`로 바꾸는 테스트: 약 4026.53초(약 67.1분)라는 **인위적인 비용**으로 C2가 선택되지 않도록 유도한다. 실제 복귀시간을 측정한 값이 아니다. governor·다른 진입 경로에 대한 보편적 하드 차단 보장은 별도로 확인해야 한다.
- `c2=1`: 공유된 실행 대화에서는 실험 모듈의 옵션으로 사용되었다. **표준 Linux cpuidle 커널 옵션이라는 의미는 아니다.** 구현 코드에서 파라미터 의미를 확인해야 한다.
- `local-timer-stop`: DT의 idle 상태에서 로컬 타이머 정지를 표시하는 속성이다. Linux의 타이머 broadcast 처리를 돕지만, PSCI/PMU의 실제 CPU wakeup 기능까지 보장하지는 않는다.

## 2. 확인된 실행 기록 (관찰과 해석의 분리)

출처 A: 저장소의 [Linux 5.10 부팅 기록 11.12~11.13](E85096-LINUX510-BOOT.md).
출처 B: 2026-11-02 Claude Code 대화 화면에서 사용자가 공유한 로그(원시 UART 전체 로그 및 정확한 코드 변경분은 이 문서에 첨부하지 않음).

| 시점 / 구성 | 관찰된 내용 | 판단의 한계 |
| --- | --- | --- |
| 기존 8코어 + `cpuidle.off=1` | 정상 Debian 부팅과 장시간 CPU 부하 통과(저장소 11.11) | C2는 이 조건에서 시험되지 않음 |
| 11.12: `cpuidle.off=1`만 제거 | 부팅 약 1.93초에서 정지; 전원 재인가 후 fastboot 통해 원본 이미지 복구 | 최초 한 번의 실험; 정지 원인 C2로 확정할 수 없음 |
| C2 선택 회피, `cpuidle.off=1` 제거 | 사용자가 공유한 Claude 결과: Debian 부팅 성공, psci_idle 구동, C2 usage=0/WFI 사용 | C2 진입을 막은 상태의 성공이며 C2 복귀 검증은 아님 |
| C2 활성화, `local-timer-stop` 미설정 상태 | 공유된 Claude 분석: CPU3가 C2로 들어간 뒤 타이머/RCU 진행 정체 | 타이머가 단독 근본 원인인지 미확인 |
| DT C2에 `local-timer-stop` 추가, broadcast 타이머 확인 후 C2 활성화 | 공유된 Claude 분석: 타이머 정체 증상이 달라졌지만 CPU4의 IPI 응답 정지가 관찰됨 | broadcast 장치 존재만으로 IPI 및 CPU wakeup이 정상이라는 뜻은 아님 |
| 11.13: 별도 hotplug 경로 | `e850-cpuhp` 모듈로 CPU1~7 off/on 30회 + 무작위 토글 300회, 실패 0으로 기록 | **hotplug 성공은 C2 idle 성공의 증거가 아님** |

## 3. 현재 코드 및 공개 자료에서 얻는 단서

현재 저장소의 [e850-cpuhp 모듈](../modules/e850-cpuhp/e850-cpuhp.c)은 hotplug 이벤트에 맞춰 PMU 및 interrupt generator를 조작한다.

- `CPU_INFORM`: 저전력 상태 관련 정보.
- `CLUSTERx_CPUy_INT_EN` bit 3: 기존 hotplug 모듈에서는 CPU offline/online 시 설정/해제.
- `GRP2_ENABLE`, `GRP2_UPEND`, `GRP2_CLEAR`: CPU wakeup 관련 interrupt generator 제어.
- `CPUHP_BP_PREPARE_DYN` 및 `CPUHP_AP_ONLINE_DYN`: hotplug 생명주기에서 PMU 제어가 호출되는 위치.
- PSCI `arm,psci-suspend-param`, local timer / broadcast 및 IPI: C2 진입·복귀를 이해할 때 별도로 확인할 요소.

공개 근거: Alexey Klimov의 Exynos PMU/hotplug v7 시리즈(2026-09-30)는 Exynos850 CPU hotplug 및 범용 PMU callback을 다루고, **Exynos850 idle 상태 활성화는 올바른 펌웨어가 확보된 뒤 가능한 후속 단계**라고 명시했다. 즉 공개 패치만으로 본 보드의 C2를 지원한다고 단정하지 않아야 한다.

- [Linaro Exynos850 hotplug v7 cover letter (2026-09-30)](https://lkml.iu.edu/hypermail/linux/kernel/2609.3/13895.html)
- [Exynos PMU error handling fixes v3 (2026-09-30)](https://lists.openwall.net/linux-kernel/2026/09/29/3002)
- [Exynos 공통 cpu idle/hotplug 변경 v7 3/6](https://marc.info/?l=devicetree&m=179072724663273&w=1)
- [저장소 11.13 hotplug 기록](E85096-LINUX510-BOOT.md)

## 4. 현재 결론과 주의할 점

**관찰상 결론:** C2 선택을 회피하면 `cpuidle.off=1` 없이도 부팅에 성공했다. C2를 실제로 허용하면 타이머/RCU, 이후 IPI 응답 관련 정지가 보고되었다. 따라서 **C2 관련 idle/wakeup 경로가 유력한 장애 구간**이다. 단, 원인은 PSCI/BL31, PMU interrupt mask, 타이머 broadcast, CPU IPI wakeup 중 어디인지 확정되지 않았다.

**중요한 구분:** 
1. `local-timer-stop` 선언은 Linux에 정지 특성을 알릴 뿐, CPU가 실제로 wakeup되도록 펌웨어를 고치는 수정이 아니다.
2. hotplug에서 성공한 bit3·GRP2 레지스터 쓰기를 idle 진입/복귀에 그대로 옮기는 것은 안전성이 입증되지 않았다.
3. 정상 이미지와 복구 경로를 보존해야 한다. 부팅 이전에 멈추면 사용자 공간 watchdog 또는 fastboot 자동 복구가 작동하지 않을 수 있다.
4. C2 사용 횟수 0은 C2 성공이 아니라 **미진입**의 증거이다.
5. 2026-11-02 공유 대화의 후반 분석은 Claude가 보고한 상태이며 이 문서에서 UART 로그, sysfs 카운터, 실기기 동작을 독립 검증한 것은 아니다.

**개발 맥락:** 카메라 OV5647/CSI-DMA 영상 획득 경로와 C2 안정성은 별개의 이슈로 구분할 수 있다. 정상 부팅 기준선의 카메라 진행 상태는 [Linux 5.10 부팅 기록](E85096-LINUX510-BOOT.md)에 보존되어 있다.

## 5. 문서 이용 범위

이 문서는 Claude Code가 관련 작업을 하다가 필요할 때 읽을 수 있는 **배경·근거 메모**이다. 새 커밋, 레지스터 변경, 이미지 굽기, C2 활성화, 보드 재부팅 등 **실행을 요구하지 않는다.** 관측값과 확정 사실을 구분하여 해석하기 위한 참고 기록으로만 사용한다.
