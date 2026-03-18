# 🏎️ SoCar: Zynq-7020 SoC 기반 장애물 회피 자동차 게임 (Car Dodge Game)

## 📌 Project Overview
본 프로젝트는 Xilinx Zynq-7020 SoC 보드를 활용하여 하드웨어(PL)와 소프트웨어(PS)를 연동해 구현한 아케이드 자동차 게임입니다. 각종 주변 장치(TFT-LCD, Text LCD, 7-Segment, SD Card, UART 등)를 제어하고, 인터럽트 기반의 사용자 입력 처리 및 프레임(FPS) 동기화를 최적화하는 데 중점을 두었습니다.

* **개발 보드:** Zynq-7020
* **주요 사용 디바이스:** TFT-LCD(게임 화면), Text LCD(사용자 및 최고 점수), 7-Segment(실시간 점수), Pushbutton(조작), UART(사용자 입력), SD Card(에셋 및 데이터 저장)

<br>

## ⚙️ System Workflow
게임의 전체적인 로직 및 하드웨어 제어 흐름은 다음과 같습니다.

### 1. 초기화 및 로그인 (Boot & Login)
* **SD Card & Text LCD:** SD 카드에 저장된 최고 점수와 해당 플레이어의 이름을 불러와 Text LCD에 출력합니다.
* **UART 통신:** UART를 통해 현재 플레이어의 이름(User Name)을 입력받아 Text LCD에 업데이트합니다.
* **대기 상태:** TFT-LCD에 배경, 캐릭터, 타이틀 이미지를 순서대로 렌더링하고 Pushbutton 입력 대기 무한 루프(`while(button==0)`)에 진입합니다.

### 2. 게임 실행 및 화면 렌더링 (Main Game Loop)
* **Start & Countdown:** 버튼 입력 시 타이틀을 지우고 `3, 2, 1, Go!` 카운트다운을 진행합니다. 이때 오작동 방지를 위해 `interrupt_enable = 0`으로 설정하여 입력을 차단하고, 완료 후 다시 활성화합니다.
* **장애물 생성:** 3개의 레인(좌, 중, 우)에 각각 10프레임으로 구성된 장애물 이미지 세트(`bg[3][10]`)를 준비합니다. `xtime`을 이용해 0~2의 난수를 생성하여 장애물 위치(`obs_locate`)를 결정합니다.
* **프레임 동기화:** Delay 함수(`for(i=0; i<70000000; i++);`)를 통해 초당 프레임 속도를 조절하며 장애물이 다가오는 애니메이션(`current_file` 증가)을 구현합니다. 특정 색상을 투명 처리하는 함수를 적용해 캐릭터와 배경을 자연스럽게 합성합니다.
* **점수 및 난이도 조절:** 루프 반복 횟수(`loop_count`)를 점수로 산정하여 7-Segment에 실시간 출력합니다. 점수가 50, 120, 200에 도달할 때마다 Speed-up 이미지를 띄우고 Delay 값을 줄여 난이도를 높입니다.
  * *최적화 포인트:* 점수가 120 이상이 되면 이미지 로드 병목 현상(SD 카드 읽기 속도 한계)을 방지하기 위해 `current_file + 2`씩 건너뛰며 5장만 렌더링하여 고속 주행을 부드럽게 구현했습니다.

### 3. 사용자 조작 및 충돌 판정 (Control & Collision)
* **Pushbutton 인터럽트:** 버튼 입력 시 인터럽트가 발생하여 `button` 신호에 값을 인가하고, 이에 따라 캐릭터 위치(`char_locate`)를 변경합니다.
* **Pause (일시정지):** 게임 중 3번 버튼을 누르면 일시정지 되며, 다시 누르면 카운트다운 후 재개됩니다.
* **Collision 판정:** 장애물 이미지가 캐릭터에 근접한 상태(`current_file >= 8`)에서 `char_locate`와 `obs_locate`가 일치하면 `Game Over` 함수를 호출합니다.

### 4. 종료 및 저장 (Game Over)
* Game Over 이미지를 띄우고 약 0.7초간 인터럽트를 비활성화하여 오입력을 방지합니다.
* UART로 최종 점수를 전송하고, `SaveScores` 함수를 통해 사용자 이름과 점수를 SD 카드에 안전하게 기록한 뒤 초기 타이틀 화면으로 복귀합니다.

<br>

## 🛠️ Troubleshooting & Optimization (핵심 문제 해결 경험)

**Issue: SD 카드 파일 입출력(FOPEN)과 인터럽트(ISR) 충돌 문제**
초기 설계에서는 Pushbutton 인터럽트 발생 시 호출되는 Service Routine(ISR) 내부에서 곧바로 캐릭터 이동 이미지를 로드하여 화면에 띄우도록 구현했습니다. 하지만 테스트 과정에서 다음과 같은 치명적인 문제가 발생했습니다.

1. **`fopen fail` 에러:** 메인 루프에서 SD 카드의 배경 이미지를 불러오는(`fopen`) 도중에 인터럽트가 발생하여 ISR 내부에서 다시 `fopen`을 시도하면 시스템 파일 입출력 충돌로 에러가 발생했습니다.
2. **키 씹힘(Input Drop) 현상:** 충돌을 막기 위해 `fopen ~ fclose` 구간에서 인터럽트 동작을 강제로 멈추게(Masking) 했더니, 게임 속도가 빨라질수록 사용자의 버튼 입력이 무시되는 현상이 빈번하게 발생했습니다.

**Resolution: 상태 변수(State Variable) 기반의 아키텍처 분리**
인터럽트 루틴에서 무거운 작업(파일 입출력 및 디스플레이 제어)을 직접 수행하는 것이 병목의 원인임을 파악했습니다. 
이를 해결하기 위해 **ISR에서는 오직 상태 변수(`button` 값, `char_locate`)만 업데이트하고 즉시 복귀**하도록 경량화했습니다. 무거운 이미지 로드 및 화면 렌더링 함수는 메인 루프(배경 렌더링 루프) 내부로 통합하여, 메인 루프가 순차적으로 파일 입출력을 처리하도록 아키텍처를 전면 수정했습니다. 
그 결과 파일 시스템 충돌을 완벽히 해결함과 동시에, 고속 진행 시에도 키 씹힘 없는 쾌적한 조작감을 확보할 수 있었습니다.

## Block diagram

<img width="1850" height="1247" alt="image" src="https://github.com/user-attachments/assets/ac984be8-caa4-476a-baa7-486eece18728" />


## Play

https://github.com/user-attachments/assets/83687492-10f7-4c09-bda3-6feb62ee4f37



