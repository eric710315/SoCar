#include <stdio.h>
#include "xil_printf.h"
#include "xil_io.h"
#include "xparameters.h"
#include "ff.h"
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "xil_exception.h"
#include "xscugic.h"
#include "pushbutton.h"
#include "textlcd.h"
#include "seven_seg.h"
#include "xtime_l.h"

// UART includes
#include "xil_types.h"
#include "xuartps_hw.h"

// UART defines
#define CR              0x0D
#define NAME_MAX        10
#define SP              0x20

// LCD defines
#define TextLine1       0
#define TextLine2       1
#define MAX             17

#define INTC_DEVICE_ID      XPAR_SCUGIC_0_DEVICE_ID
#define INTC_DEVICE_INT_ID  31
#define TRANSPARENT_COLOR   0xFFFF

// 점수 및 사용자 시스템 (시간 관련 변수 제거)
char username[NAME_MAX] = {0};
int current_score = 0;          // loop_count 기반 점수
int user_best_score = 0;
int global_best_score = 0;
int system_ready = 0;           // 시스템 준비 완료 (로그인 완료)

// 게임 파일들
static FATFS fatfs;
static FIL fil;
static char score_file[32] = "scores.txt";

static char bg1[4][32] = {"bg1.bin", "bg2.bin", "bg3.bin", "bg4.bin"};
static char bg[3][10][32] = {
    {"bg10.bin", "bg11.bin", "bg12.bin", "bg13.bin", "bg14.bin", "bg15.bin", "bg16.bin", "bg17.bin", "bg18.bin", "bg19.bin"},
    {"bg20.bin", "bg21.bin", "bg22.bin", "bg23.bin", "bg24.bin", "bg25.bin", "bg26.bin", "bg27.bin", "bg28.bin", "bg29.bin"},
    {"bg30.bin", "bg31.bin", "bg32.bin", "bg33.bin", "bg34.bin", "bg35.bin", "bg36.bin", "bg37.bin", "bg38.bin", "bg39.bin"}
};

static char count[4][32] = {"count3.bin", "count2.bin", "count1.bin", "count4.bin"};
static char char_file[32] = "char.bin";
static char gameover[32] = "gameover.bin";
static char title[32] = "title.bin";
static char pause[32] = "pause.bin";
static char speedup[32] = "speedup.bin";

// 게임 변수들
static int Data;
static int R, G, B;
int current_file = 0;
int current_file_c = 0;
int char_x = 100;
int game_state = 0;
int obs_locate;
int button = 0;
int loop_count = 0;             // 이것이 점수가 됨
int start = 0;
int interrupt_enable = 1;
int char_locate = 1;

FRESULT Res;
TCHAR *Path = "0:/";
u32 * buffer[65280];
u32 data_size = 4 * 65280;
u32 NumBytesRead;

// 인터럽트 컨트롤러
XScuGic InterruptController;
static XScuGic_Config *GicConfig;

// 함수 프로토타입
void SD_read(char *filename);
void TFTLCD_default();
void TFTLCD_write_background_main();
void TFTLCD_write_background_main_count();
void TFTLCD_write_background_in_char();
void TFTLCD_write_background_in_char_count();
void TFTLCD_write_background_obs_level01();
void TFTLCD_write_background_obs_level2();
void TFTLCD_write_char();
void TFTLCD_write_gameover();
void TFTLCD_write_title();
void TFTLCD_write_countdown();
void TFTLCD_write_pause();
void TFTLCD_write_speedup();
void game_over();
void speed_up();
void delay_07sec();
void delay_03sec();
void delay_01sec();
void delay_005sec();
void delay_003sec();

// 사용자 시스템 함수들 (시간 관련 함수 제거)
void InitUART();
void InitMsg();
void PrintChar(char *str);
void GetUsername(char *username);
void LoadScores();
void SaveScores();
void UpdateTextLCD();
void UpdateSevenSegment();
void ResetScore();

// 인터럽트 함수들
int GicConfigure(u16 DeviceId);
void ServiceRoutine(void *CallbackRef);

static inline void draw_pixel(int x, int y, uint16_t pix) {
    uint16_t R = (pix >> 11) & 0x1F;
    uint16_t G = (pix >>  5) & 0x3F;
    uint16_t B = (pix      ) & 0x1F;
    uint16_t out = (B << 11) | (G << 5) | R;
    uint32_t addr = XPAR_TFTLCD_0_S00_AXI_BASEADDR + (x + 480*y) * 4;
    Xil_Out32(addr, out);
}

int main() {
    // 시드 초기화
    XTime t;
    XTime_GetTime(&t);
    srand((unsigned int)t);

    // UART 초기화
    InitUART();
    InitMsg();

    // 초기 LCD 메시지
    char welcome_line1[17];
    char welcome_line2[17];
    sprintf(welcome_line1, "  Welcome to SoCar ");
    sprintf(welcome_line2, " Please Log in ");

    int i;
    for(i = 0; i < 4; i++) {
        TEXTLCD_mWriteReg(XPAR_TEXTLCD_0_S00_AXI_BASEADDR, i*4,
                         welcome_line1[i*4+3] + (welcome_line1[i*4+2]<<8) + (welcome_line1[i*4+1]<<16) + (welcome_line1[i*4]<<24));
        TEXTLCD_mWriteReg(XPAR_TEXTLCD_0_S00_AXI_BASEADDR, i*4+16,
                         welcome_line2[i*4+3] + (welcome_line2[i*4+2]<<8) + (welcome_line2[i*4+1]<<16) + (welcome_line2[i*4]<<24));
    }

    // 로그인 전 세그먼트 초기화 (0점으로 고정)
    UpdateSevenSegment();

    // 사용자 이름 입력
    GetUsername(username);

    // 점수 로딩
    LoadScores();

    // LCD 업데이트
    UpdateTextLCD();

    // 시스템 준비 완료
    system_ready = 1;

    // 인터럽트 설정
    int Status = GicConfigure(INTC_DEVICE_ID);
    if (Status != XST_SUCCESS) {
        xil_printf("GIC Configure Failed\r\n");
        return XST_FAILURE;
    }

    // 메인 게임 루프
    while(1) {
        interrupt_enable = 1;
        start = 0;
        button = 0;

        // 점수 초기화
        ResetScore();

        obs_locate = rand() % 3;

        TFTLCD_default();

        // 게임 시작 대기 (0점 고정 표시)
        while(button == 0) {
            UpdateSevenSegment(); // 0점 고정 표시
        }
        interrupt_enable = 0;

        button = 0;
        TFTLCD_write_background_main();
        TFTLCD_write_char();
        TFTLCD_write_countdown();  // 카운트다운 실행

        // 카운트다운 완료 후 게임 시작
        interrupt_enable = 1;
        start = 1;

        // 레벨별 게임 진행
        if (loop_count < 120) {
            TFTLCD_write_background_obs_level01();
        }
        if (loop_count >= 120) {
            TFTLCD_write_background_obs_level2();
        }
    }

    return XST_SUCCESS;
}

// 점수 초기화 (시간 관련 함수 대체)
void ResetScore() {
    loop_count = 0;
    current_score = 0;
    //xil_printf("Score reset to 0\n");
}

// 수정된 7 segment 표시 함수 (Verilog 코드에 맞춤)
void UpdateSevenSegment() {
    // loop_count를 점수로 사용
    current_score = loop_count;

    // 4자리 숫자를 개별 자릿수로 분리
    int thousands = (current_score / 1000) % 10;
    int hundreds = (current_score / 100) % 10;
    int tens = (current_score / 10) % 10;
    int ones = current_score % 10;

    // Verilog 코드에 맞는 32비트 데이터 구성
    // data[31:28] → seg7, data[27:24] → seg6, data[23:20] → seg5, data[19:16] → seg4
    // data[15:12] → seg3, data[11:8] → seg2, data[7:4] → seg1, data[3:0] → seg0
    // 4개 세그먼트만 사용: seg3(천의자리), seg2(백의자리), seg1(십의자리), seg0(일의자리)
    u32 SegReg = (0xF << 28) +          // seg7 (비활성화)
                 (0xF << 24) +          // seg6 (비활성화)
                 (0xF << 20) +          // seg5 (비활성화)
                 (0xF << 16) +          // seg4 (비활성화)
                 (thousands << 12) +    // seg3 (천의자리)
                 (hundreds << 8) +      // seg2 (백의자리)
                 (tens << 4) +          // seg1 (십의자리)
                 (ones);                // seg0 (일의자리)

    SEVEN_SEG_mWriteReg(XPAR_SEVEN_SEG_0_S00_AXI_BASEADDR,
                        SEVEN_SEG_S00_AXI_SLV_REG0_OFFSET, SegReg);

    // 디버깅 출력

}

// UART 시스템
void InitUART() {
    u32 CntrlRegister;
    CntrlRegister = XUartPs_ReadReg(XPAR_PS7_UART_1_BASEADDR, XUARTPS_CR_OFFSET);
    XUartPs_WriteReg(XPAR_PS7_UART_1_BASEADDR, XUARTPS_CR_OFFSET,
                    ((CntrlRegister & ~XUARTPS_CR_EN_DIS_MASK) | XUARTPS_CR_TX_EN | XUARTPS_CR_RX_EN));
}

void InitMsg() {
    PrintChar("==Welcome to SoCar==");
    PrintChar("Please enter your username: ");
}

void PrintChar(char *str) {
    XUartPs_SendByte(XPAR_PS7_UART_1_BASEADDR, CR);
    while(*str != 0) {
        XUartPs_SendByte(XPAR_PS7_UART_1_BASEADDR, *str++);
    }
}

void GetUsername(char *username) {
    int i = 0;
    char tmp;

    do {
        tmp = XUartPs_RecvByte(XPAR_PS7_UART_1_BASEADDR);
        if(((tmp >= 'A') && (tmp <= 'Z')) || ((tmp >= 'a') && (tmp <= 'z')) ||
           ((tmp >= '0') && (tmp <= '9')) || (tmp == '_')) {
            XUartPs_SendByte(XPAR_PS7_UART_1_BASEADDR, tmp);
            username[i] = tmp;
            i++;
        }
    } while(!((tmp == CR) || (i >= NAME_MAX-1)));

    username[i] = '\0';
    while(tmp != CR)
        tmp = XUartPs_RecvByte(XPAR_PS7_UART_1_BASEADDR);
    PrintChar("Game starting...\n");
}

// 점수 시스템
void LoadScores() {
    char buffer[1024]; // 더 큰 버퍼
    char file_username[NAME_MAX];
    int file_score;
    UINT bytes_read;

    user_best_score = 0;
    global_best_score = 0;

    Res = f_mount(&fatfs, Path, 0);
    if(Res != FR_OK) return;

    Res = f_open(&fil, score_file, FA_READ);
    if(Res) return;

    if(f_read(&fil, buffer, sizeof(buffer)-1, &bytes_read) == FR_OK && bytes_read > 0) {
        buffer[bytes_read] = '\0';
        char *line = strtok(buffer, "\n\r");

        while(line != NULL) {
            if(sscanf(line, "%s %d", file_username, &file_score) == 2) {
                // 현재 사용자의 최고점수 찾기 (여러 기록 중 최고점)
                if(strcmp(file_username, username) == 0 && file_score > user_best_score) {
                    user_best_score = file_score;
                }
                // 전체 최고점수 찾기
                if(file_score > global_best_score) {
                    global_best_score = file_score;
                }
            }
            line = strtok(NULL, "\n\r");
        }
    }
    f_close(&fil);

    xil_printf("Loaded scores - User: %d, Global: %d\n", user_best_score, global_best_score);
}


void SaveScores() {
    if(current_score <= user_best_score) return;

    user_best_score = current_score;

    Res = f_mount(&fatfs, Path, 0);
    if(Res != FR_OK) return;

    // 검색 결과[3] 방식: FA_OPEN_APPEND 사용
    Res = f_open(&fil, score_file, FA_OPEN_APPEND | FA_WRITE);
    if(Res != FR_OK) {
        // 파일이 없으면 새로 생성
        Res = f_open(&fil, score_file, FA_CREATE_NEW | FA_WRITE);
        if(Res != FR_OK) return;
    }

    // 사용자명과 점수를 파일 끝에 추가
    char buffer[64];
    sprintf(buffer, "%s %d\n", username, user_best_score);
    UINT bytes_written;
    f_write(&fil, buffer, strlen(buffer), &bytes_written);
    f_close(&fil);

    // 전체 최고점수 업데이트를 위해 다시 로딩
    LoadScores();
    UpdateTextLCD();


}


void UpdateTextLCD() {
    char line1[17];
    char line2[17];
    int i;

    sprintf(line1, "%-8s  : %4d", username, user_best_score);
    sprintf(line2, "HIGHSCORE : %4d", global_best_score);

    for(i = 0; i < 4; i++) {
        TEXTLCD_mWriteReg(XPAR_TEXTLCD_0_S00_AXI_BASEADDR, i*4,
                         line1[i*4+3] + (line1[i*4+2]<<8) + (line1[i*4+1]<<16) + (line1[i*4]<<24));
        TEXTLCD_mWriteReg(XPAR_TEXTLCD_0_S00_AXI_BASEADDR, i*4+16,
                         line2[i*4+3] + (line2[i*4+2]<<8) + (line2[i*4+1]<<16) + (line2[i*4]<<24));
    }
}

// SD 카드 읽기
void SD_read(char *filename) {
    Res = f_mount(&fatfs, Path, 0);
    if(Res != FR_OK){
        xil_printf("\nmount_fail\n");
        return;
    }

    Res = f_open(&fil, filename, FA_READ);
    if(Res){
        xil_printf("\nfile_open_fail\n");
        return;
    }

    Res = f_lseek(&fil, 0);
    if (Res) {
        xil_printf("\nfseek_fail\n");
        return;
    }

    Res = f_read(&fil, buffer, data_size, &NumBytesRead);
    if (Res){
        xil_printf("\ndata_read_fail\n");
        return;
    }

    Res = f_close(&fil);
}

// 게임 화면 함수들
void TFTLCD_default() {
    TFTLCD_write_background_main();
    char_x = 100;
    TFTLCD_write_char();
    char_locate = 1;
    TFTLCD_write_title();
}

void TFTLCD_write_background_main() {
    SD_read(bg1[0]);
    for (int i = 0; i < 272; i++){
        for (int j = 0; j < 240; j++){
            Data = (int)buffer[j + 240*i] & 0x0000ffff;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (2*j + 480*i)*4, Data);

            Data = (int)buffer[j + 240*i] >> 16;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (1 + 2*j + 480*i)*4, Data);
        }
    }
}

void TFTLCD_write_background_main_count() {
    SD_read(bg1[0]);
    for (int i = 70; i < 272; i++){
        for (int j = 0; j < 240; j++){
            Data = (int)buffer[j + 240*i] & 0x0000ffff;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (2*j + 480*i)*4, Data);

            Data = (int)buffer[j + 240*i] >> 16;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (1 + 2*j + 480*i)*4, Data);
        }
    }
}

void TFTLCD_write_background_in_char() {
    SD_read(bg[obs_locate][current_file]);
    for (int i = 0; i < 272; i++){
        for (int j = 0; j < 240; j++){
            Data = (int)buffer[j + 240*i] & 0x0000ffff;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (2*j + 480*i)*4, Data);

            Data = (int)buffer[j + 240*i] >> 16;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (1 + 2*j + 480*i)*4, Data);
        }
    }
}

void TFTLCD_write_background_in_char_count() {
    SD_read(bg[obs_locate][current_file]);
    for (int i = 90; i < 272; i++){
        for (int j = 0; j < 240; j++){
            Data = (int)buffer[j + 240*i] & 0x0000ffff;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (2*j + 480*i)*4, Data);

            Data = (int)buffer[j + 240*i] >> 16;
            R = (Data >> 11) & 0x0000001f;
            G = Data & 0x000007E0;
            B = Data & 0x0000001f;
            Data = (B<<11)| G | R;
            Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (1 + 2*j + 480*i)*4, Data);
        }
    }
}

// 수정된 게임 루프 (loop_count 기반 점수)
void TFTLCD_write_background_obs_level01() {
    while(1) {
        // 점수 업데이트 (loop_count 기반)
        UpdateSevenSegment();

        Res = f_open(&fil, bg[obs_locate][current_file], FA_READ);
        if(Res){
            xil_printf("file_open_fail\n");
            current_file = 0;
            continue;
        }

        Res = f_lseek(&fil, 0);
        if (Res) {
            xil_printf("fseek_fail\n");
            f_close(&fil);
            current_file = 0;
            continue;
        }

        Res = f_read(&fil, buffer, data_size, &NumBytesRead);
        if(Res){
            xil_printf("data_read_fail\n");
            f_close(&fil);
            current_file = 0;
            continue;
        }
        f_close(&fil);

        for (int i = 0; i < 272; i++){
            for (int j = 0; j < 240; j++){
                if (j >= char_x && j < char_x + 40 && i >= 15 && i < 70) {
                    continue;
                }

                Data = (int)buffer[j + 240*i] & 0x0000ffff;
                R = (Data >> 11) & 0x0000001f;
                G = Data & 0x000007E0;
                B = Data & 0x0000001f;
                Data = (B<<11)| G | R;
                Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (2*j + 480*i)*4, Data);

                Data = (int)buffer[j + 240*i] >> 16;
                R = (Data >> 11) & 0x0000001f;
                G = Data & 0x000007E0;
                B = Data & 0x0000001f;
                Data = (B<<11)| G | R;
                Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (1 + 2*j + 480*i)*4, Data);
            }
        }

        current_file = (current_file + 1);

        if (current_file >= 8){
            if (char_locate == obs_locate){
                game_over(); // 게임 오버
                current_file = 0;
                obs_locate = rand() % 3;
                break;
            }
            if (current_file == 10){
                current_file = 0;
                obs_locate = rand() % 3;
            }
        }

        // 버튼 처리
        if ((button == 1) && (game_state == 1)){
            if ((char_locate) == 1){
                TFTLCD_write_background_in_char();
                char_x = 163;
                TFTLCD_write_char();
                char_locate = 0;
            }
            else if ((char_locate) == 2){
                TFTLCD_write_background_in_char();
                char_x = 100;
                TFTLCD_write_char();
                char_locate = 1;
            }
        }
        else if ((button == 2) && (game_state == 1)){
            if ((char_locate) == 0){
                TFTLCD_write_background_in_char();
                char_x = 100;
                TFTLCD_write_char();
                char_locate = 1;
            }
            else if ((char_locate) == 1){
                TFTLCD_write_background_in_char();
                char_x = 36;
                TFTLCD_write_char();
                char_locate = 2;
            }
        }

        // 일시정지 기능 (단순화)
        if (button == 3){
            TFTLCD_write_pause();
            game_state = 0;
            while((game_state == 0) || (button != 3)){
                UpdateSevenSegment(); // 점수 유지
            }

            interrupt_enable = 0;
            TFTLCD_write_background_in_char_count();
            TFTLCD_write_countdown();
            interrupt_enable = 1;
        }

        button = 0;
        if (loop_count < 50){
            delay_01sec();
        }
        else if (loop_count >= 50){
            delay_005sec();
        }
        loop_count = loop_count + 1; // 점수 증가

        if (loop_count == 50){
            speed_up();
        }
        if (loop_count == 120){
            speed_up();
            break;
        }
    }
}

void TFTLCD_write_background_obs_level2() {
    while(1) {
        // 점수 업데이트 (loop_count 기반)
        UpdateSevenSegment();

        Res = f_open(&fil, bg[obs_locate][current_file], FA_READ);
        if(Res){
            xil_printf("file_open_fail\n");
            current_file = 0;
            continue;
        }

        Res = f_lseek(&fil, 0);
        if (Res) {
            xil_printf("fseek_fail\n");
            f_close(&fil);
            current_file = 0;
            continue;
        }

        Res = f_read(&fil, buffer, data_size, &NumBytesRead);
        if(Res){
            xil_printf("data_read_fail\n");
            f_close(&fil);
            current_file = 0;
            continue;
        }
        f_close(&fil);

        for (int i = 0; i < 272; i++){
            for (int j = 0; j < 240; j++){
                if (j >= char_x && j < char_x + 40 && i >= 15 && i < 70) {
                    continue;
                }

                Data = (int)buffer[j + 240*i] & 0x0000ffff;
                R = (Data >> 11) & 0x0000001f;
                G = Data & 0x000007E0;
                B = Data & 0x0000001f;
                Data = (B<<11)| G | R;
                Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (2*j + 480*i)*4, Data);

                Data = (int)buffer[j + 240*i] >> 16;
                R = (Data >> 11) & 0x0000001f;
                G = Data & 0x000007E0;
                B = Data & 0x0000001f;
                Data = (B<<11)| G | R;
                Xil_Out32(XPAR_TFTLCD_0_S00_AXI_BASEADDR + (1 + 2*j + 480*i)*4, Data);
            }
        }

        current_file = (current_file + 2);

        if (current_file >= 9){
            if (char_locate == obs_locate){
                game_over(); // 게임 오버
                current_file = 0;
                obs_locate = rand() % 3;
                break;
            }
            if (current_file == 10){
                current_file = 0;
                obs_locate = rand() % 3;
            }
        }

        // 버튼 처리 (동일)
        if ((button == 1) && (game_state == 1)){
            if ((char_locate) == 1){
                TFTLCD_write_background_in_char();
                char_x = 163;
                TFTLCD_write_char();
                char_locate = 0;
            }
            else if ((char_locate) == 2){
                TFTLCD_write_background_in_char();
                char_x = 100;
                TFTLCD_write_char();
                char_locate = 1;
            }
        }
        else if ((button == 2) && (game_state == 1)){
            if ((char_locate) == 0){
                TFTLCD_write_background_in_char();
                char_x = 100;
                TFTLCD_write_char();
                char_locate = 1;
            }
            else if ((char_locate) == 1){
                TFTLCD_write_background_in_char();
                char_x = 36;
                TFTLCD_write_char();
                char_locate = 2;
            }
        }

        // 일시정지 기능
        if (button == 3){
            TFTLCD_write_pause();
            game_state = 0;
            while((game_state == 0) || (button != 3)){
                UpdateSevenSegment(); // 점수 유지
            }

            interrupt_enable = 0;
            TFTLCD_write_background_in_char_count();
            TFTLCD_write_countdown();
            interrupt_enable = 1;
        }

        button = 0;
        if (loop_count < 200){
            delay_005sec();
        }
        else if (loop_count >= 200){
            delay_003sec();
        }
        loop_count = loop_count + 1; // 점수 증가

        if (loop_count == 200){
            speed_up();
        }
    }
}

// 나머지 화면 함수들
void TFTLCD_write_char() {
    bool transparent = true;
    SD_read(char_file);
    for (int y = 15; y < 70; ++y) {
        for (int j = char_x; j < char_x + 50; ++j) {
            uint32_t raw = buffer[j + 240*y];
            uint16_t pix1 = raw & 0xFFFF;
            if (!transparent || pix1 != TRANSPARENT_COLOR) {
                draw_pixel(2*j, y, pix1);
            }
            uint16_t pix2 = raw >> 16;
            if (!transparent || pix2 != TRANSPARENT_COLOR) {
                draw_pixel(2*j+1, y, pix2);
            }
        }
    }
}

void TFTLCD_write_gameover() {
    bool transparent = true;
    SD_read(gameover);
    for (int y = 0; y < 272; ++y) {
        for (int j = 0; j < 240; ++j) {
            uint32_t raw = buffer[j + 240*y];
            uint16_t pix1 = raw & 0xFFFF;
            if (!transparent || pix1 != TRANSPARENT_COLOR) {
                draw_pixel(2*j, y, pix1);
            }
            uint16_t pix2 = raw >> 16;
            if (!transparent || pix2 != TRANSPARENT_COLOR) {
                draw_pixel(2*j+1, y, pix2);
            }
        }
    }
}

void TFTLCD_write_title() {
    bool transparent = true;
    SD_read(title);
    for (int y = 0; y < 272; ++y) {
        for (int j = 0; j < 240; ++j) {
            uint32_t raw = buffer[j + 240*y];
            uint16_t pix1 = raw & 0xFFFF;
            if (!transparent || pix1 != TRANSPARENT_COLOR) {
                draw_pixel(2*j, y, pix1);
            }
            uint16_t pix2 = raw >> 16;
            if (!transparent || pix2 != TRANSPARENT_COLOR) {
                draw_pixel(2*j+1, y, pix2);
            }
        }
    }
}

void TFTLCD_write_countdown() {
    bool transparent = true;
    while(1){
        SD_read(count[current_file_c]);
        for (int y = 0; y < 272; ++y) {
            for (int j = 0; j < 240; ++j) {
                uint32_t raw = buffer[j + 240*y];
                uint16_t pix1 = raw & 0xFFFF;
                if (!transparent || pix1 != TRANSPARENT_COLOR) {
                    draw_pixel(2*j, y, pix1);
                }
                uint16_t pix2 = raw >> 16;
                if (!transparent || pix2 != TRANSPARENT_COLOR) {
                    draw_pixel(2*j+1, y, pix2);
                }
            }
        }
        current_file_c = current_file_c + 1;

        delay_07sec();
        if (start == 0){
            TFTLCD_write_background_main_count();
        }
        else if (start == 1){
            TFTLCD_write_background_in_char_count();
        }
        if (current_file_c == 4){
            current_file_c = 0;
            break;
        }
    }
}

void TFTLCD_write_pause() {
    bool transparent = true;
    SD_read(pause);
    for (int y = 0; y < 272; ++y) {
        for (int j = 0; j < 240; ++j) {
            uint32_t raw = buffer[j + 240*y];
            uint16_t pix1 = raw & 0xFFFF;
            if (!transparent || pix1 != TRANSPARENT_COLOR) {
                draw_pixel(2*j, y, pix1);
            }
            uint16_t pix2 = raw >> 16;
            if (!transparent || pix2 != TRANSPARENT_COLOR) {
                draw_pixel(2*j+1, y, pix2);
            }
        }
    }
}

void TFTLCD_write_speedup() {
    bool transparent = true;
    SD_read(speedup);
    for (int y = 0; y < 272; ++y) {
        for (int j = 0; j < 240; ++j) {
            uint32_t raw = buffer[j + 240*y];
            uint16_t pix1 = raw & 0xFFFF;
            if (!transparent || pix1 != TRANSPARENT_COLOR) {
                draw_pixel(2*j, y, pix1);
            }
            uint16_t pix2 = raw >> 16;
            if (!transparent || pix2 != TRANSPARENT_COLOR) {
                draw_pixel(2*j+1, y, pix2);
            }
        }
    }
}

// 게임 종료 (loop_count 기반)
void game_over() {
    // 터미널에 최종 결과 출력
    PrintChar("GAME OVER!");

    char score_msg[64];
    sprintf(score_msg, "Final Score: %d", current_score);
    PrintChar(score_msg);

    if(current_score == user_best_score && current_score > 0) {
        PrintChar("NEW PERSONAL BEST!");
    }

    if(current_score == global_best_score && current_score > 0) {
        PrintChar("NEW GLOBAL HIGH SCORE!");
    }

    TFTLCD_write_gameover();
    SaveScores();
    interrupt_enable = 0;
    delay_07sec();

    game_state = 0;
    while(game_state == 0){}
}

void speed_up() {
    interrupt_enable = 0;
    for (int i=0; i<3; i++){
        TFTLCD_write_background_in_char_count();
        delay_03sec();
        TFTLCD_write_speedup();
        delay_03sec();
    }
    interrupt_enable = 1;
}

// 딜레이 함수들
void delay_07sec() {
    volatile int i;
    for(i=0; i<70000000; i++);
}

void delay_03sec() {
    volatile int i;
    for(i=0; i<30000000; i++);
}

void delay_01sec() {
    volatile int i;
    for(i=0; i<10000000; i++);
}

void delay_005sec() {
    volatile int i;
    for(i=0; i<5000000; i++);
}

void delay_003sec() {
    volatile int i;
    for(i=0; i<3000000; i++);
}

// 인터럽트 설정
int GicConfigure(u16 DeviceId) {
    int Status;

    GicConfig = XScuGic_LookupConfig(DeviceId);
    if (NULL == GicConfig) {
        return XST_FAILURE;
    }

    Status = XScuGic_CfgInitialize(&InterruptController, GicConfig,
                    GicConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
            (Xil_ExceptionHandler) XScuGic_InterruptHandler,
            &InterruptController);

    Xil_ExceptionEnable();

    Status = XScuGic_Connect(&InterruptController, INTC_DEVICE_INT_ID,
               (Xil_ExceptionHandler)ServiceRoutine,
               (void *)&InterruptController);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    XScuGic_Enable(&InterruptController, INTC_DEVICE_INT_ID);

    return XST_SUCCESS;
}

void ServiceRoutine(void *CallbackRef) {
    char pb;

    pb = PUSHBUTTON_mReadReg(XPAR_PUSHBUTTON_0_S00_AXI_BASEADDR, 0);
    PUSHBUTTON_mWriteReg(XPAR_PUSHBUTTON_0_S00_AXI_BASEADDR, 0, 0);

    if ((pb & 1) == 1){
        if (interrupt_enable == 1){
            button = 1;
        }
        if (game_state == 0){
            game_state = 1;
        }
    }
    else if ((pb & 2) == 2){
        if (interrupt_enable == 1){
            button = 2;
        }
        if (game_state == 0){
            game_state = 1;
        }
    }
    else if ((pb & 4) == 4){
        if (interrupt_enable == 1){
            button = 3;
        }
        if (game_state == 0){
            game_state = 1;
        }
    }
    else if ((pb & 8) == 8){
        if (interrupt_enable == 1){
            button = 4;
        }
        if (game_state == 0){
            game_state = 1;
        }
    }
}
