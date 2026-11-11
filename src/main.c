// ===============================
// JKIT-128-1 2인 게임 (최종 수정 버전)
// 수정사항:
// 1. 배경음 속도 추가 조절 (더 천천히)
//    - 기존 80ms -> 150ms (약 0.15초) 간격으로 변경
//    - "따라라라" (빠름) -> "딴--딴--딴--딴" (여유 있고 비장함)
// ===============================

#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

// ------------------------------
// BUZZER 핀 정의 (PB4)
// ------------------------------
#define BUZZER PB4

// ------------------------------
// 주파수(음계) 정의
// ------------------------------
#define TONE_P1 880   // Player1 버튼음 (라)
#define TONE_P2 440   // Player2 버튼음 (라 - 한 옥타브 아래)

// [박진감 넘치는 BGM 음계 - 5~6옥타브]
#define NOTE_C5 523   // 도
#define NOTE_E5 659   // 미
#define NOTE_G5 784   // 솔
#define NOTE_C6 1047  // 높은 도

// [승리/패배 멜로디 (4음)]
const uint16_t win_melody[4]  = {523, 659, 784, 1047};   // 도 미 솔 도
const uint16_t lose_melody[4] = {392, 311, 261, 196};    // 솔 미b 도 솔

// [라운드 승리 효과음용 음계]
#define NOTE_WIN_C 523 // 도
#define NOTE_WIN_E 659 // 미
#define NOTE_WIN_G 784 // 솔

// [액션/추격전 BGM 배열]
// 도->미->솔->높은도 (상승 아르페지오)
const uint16_t bgm_loop[] = {
	NOTE_C5, NOTE_E5, NOTE_G5, NOTE_C6
};
#define BGM_LEN 4

// BGM 재생 관련 변수
volatile uint8_t melody_index = 0;
volatile uint8_t play_bgm = 0;
volatile uint8_t bgm_repeat_count = 0;
volatile const uint16_t* current_melody;

// 현재 효과음(버튼음/승리음)이 울리고 있는지 확인하는 플래그
volatile uint8_t is_beeping = 0;

// ------------------------------
// 7세그먼트 코드 (0~9)
// ------------------------------
const uint8_t seg_code[10] = {
	0x3F,0x06,0x5B,0x4F,0x66,
	0x6D,0x7D,0x07,0x7F,0x6F
};

// ------------------------------
// 게임 상태 변수
// ------------------------------
volatile uint8_t num1=0, num2=0;
volatile uint8_t paused1=0, paused2=0;
volatile uint8_t saved1=0, saved2=0;
volatile uint8_t score1=0, score2=0;
volatile uint8_t compare_flag=0;

volatile uint32_t last_int4=0, last_int5=0;
volatile uint32_t ms_counter = 0;

// ------------------------------
// 소프트 delay
// ------------------------------
void soft_delay_ms(uint16_t ms){
	for(uint16_t i=0;i<ms;i++){
		_delay_ms(1);
		ms_counter++;
	}
}

// ------------------------------
// 7세그먼트 출력 함수
// ------------------------------
void show_digit(uint8_t digit, uint8_t com){
	PORTG = (1<<com);
	PORTC = seg_code[digit];
	for(volatile uint16_t d=0; d<800; d++);
	PORTG = 0x00;
}

void display_4digit(uint8_t left, uint8_t right){
	uint8_t l1 = left/10;
	uint8_t l2 = left%10;
	uint8_t r1 = right/10;
	uint8_t r2 = right%10;

	show_digit(r2,0);
	show_digit(r1,1);
	show_digit(l2,2);
	show_digit(l1,3);
}

// ------------------------------
// 점수 LED 출력
// ------------------------------
void display_score_led(){
	PORTA = 0x00;
	for(uint8_t i=0; i<score1; i++) PORTA |= (0x80 >> i);
	for(uint8_t i=0; i<score2; i++) PORTA |= (0x01 << i);
}

// ------------------------------
// Timer3를 이용한 부저 설정 함수
// ------------------------------
void buzzer_set_tone(uint16_t freq){
	if(freq == 0){
		ETIMSK &= ~(1<<OCIE3A);
		TCCR3B = 0;
		TCNT3 = 0;
		PORTB &= ~(1<<BUZZER);
		} else {
		TCCR3B = 0;
		TCNT3 = 0;
		TCCR3A = 0;

		OCR3A = (uint16_t)((F_CPU/(2UL*64UL*freq))-1);

		ETIMSK |= (1<<OCIE3A);
		TCCR3B = (1<<WGM32)|(1<<CS31)|(1<<CS30);
	}
}

ISR(TIMER3_COMPA_vect){
	PORTB ^= (1<<BUZZER);
}

// ------------------------------
// 짧은 비프음 (버튼 누를 때)
// ------------------------------
void beep(uint16_t f){
	is_beeping = 1;      // 배경음 중단 요청
	buzzer_set_tone(f);
	_delay_ms(80);
	buzzer_set_tone(0);
	is_beeping = 0;      // 배경음 복귀
}

// ------------------------------
// 라운드 승리 효과음
// ------------------------------
void play_round_win_sound(uint8_t winner){
	is_beeping = 1; // 배경음 타이머 간섭 차단

	if(winner == 1){
		// Player 1 승리: 도-미-솔 (상승)
		buzzer_set_tone(NOTE_WIN_C); _delay_ms(100);
		buzzer_set_tone(NOTE_WIN_E); _delay_ms(100);
		buzzer_set_tone(NOTE_WIN_G); _delay_ms(150);
	}
	else {
		// Player 2 승리: 솔-미-도 (하강)
		buzzer_set_tone(NOTE_WIN_G); _delay_ms(100);
		buzzer_set_tone(NOTE_WIN_E); _delay_ms(100);
		buzzer_set_tone(NOTE_WIN_C); _delay_ms(150);
	}

	buzzer_set_tone(0);
	is_beeping = 0; // 차단 해제
}

// ------------------------------
// Timer0: BGM 및 배경 효과음 관리
// ------------------------------
ISR(TIMER0_COMP_vect){
	static uint8_t game_bgm_tick = 0;
	static uint8_t game_bgm_idx = 0;

	// 1. 승리/패배 BGM 재생 중일 때 (최종 결과)
	if(play_bgm){
		static uint8_t bgm_tick=0;
		bgm_tick++;

		if(bgm_tick >= 30){
			bgm_tick = 0;
			melody_index++;

			if(melody_index >= 4){
				if(bgm_repeat_count > 0){
					bgm_repeat_count--;
					melody_index = 0;
					} else {
					play_bgm = 0;
					buzzer_set_tone(0);
					return;
				}
			}
			buzzer_set_tone(current_melody[melody_index]);
		}
	}
	// 2. 효과음(버튼음/라운드승리음) 재생 중일 때는 배경음 생략
	else if(is_beeping){
		// Do nothing
	}
	// 3. 게임 진행 중일 때 - "박진감 넘치는 액션 사운드" 재생
	else if(!paused1 || !paused2){
		game_bgm_tick++;

		// [속도 조절] 15 (150ms) 간격으로 변경 (기존 80ms보다 더 느리게)
		// "딴--딴--딴--딴" 느낌으로 명확하게 재생
		if(game_bgm_tick >= 15){
			game_bgm_tick = 0;
			buzzer_set_tone(bgm_loop[game_bgm_idx]);
			game_bgm_idx++;
			if(game_bgm_idx >= BGM_LEN) game_bgm_idx = 0;
		}
	}
	// 4. 둘 다 멈췄을 때 (결과 대기 중)
	else {
		buzzer_set_tone(0);
		game_bgm_tick = 0;
		game_bgm_idx = 0;
	}
}

// ------------------------------
// INT4 = Player1 버튼
// ------------------------------
ISR(INT4_vect){
	sei();
	if(ms_counter - last_int4 < 200) return;
	last_int4 = ms_counter;

	beep(TONE_P1);

	if(!paused1){
		paused1 = 1;
		saved1 = num1;
	}
	if(paused2) compare_flag = 1;
}

// ------------------------------
// INT5 = Player2 버튼
// ------------------------------
ISR(INT5_vect){
	sei();
	if(ms_counter - last_int5 < 200) return;
	last_int5 = ms_counter;

	beep(TONE_P2);

	if(!paused2){
		paused2 = 1;
		saved2 = num2;
	}
	if(paused1) compare_flag = 1;
}

// ------------------------------
// 승패 처리 로직
// ------------------------------
void compare_and_update(){
	if(!compare_flag) return;

	// 잠시 대기
	for(uint16_t t=0;t<250;t++){
		display_4digit(saved1, saved2);
		soft_delay_ms(1);
	}

	// 점수 업데이트 및 라운드 효과음
	if(saved1 > saved2){
		if(score1 < 4) score1++;
		display_score_led();
		play_round_win_sound(1);
	}
	else if(saved2 > saved1){
		if(score2 < 4) score2++;
		display_score_led();
		play_round_win_sound(2);
	}

	// 최종 결과 멜로디 (4점)
	if(score1 == 4 || score2 == 4){
		melody_index = 0;
		play_bgm = 1;
		bgm_repeat_count = 2; // 3회 반복

		if(score1 == 4) current_melody = win_melody;
		else            current_melody = lose_melody;

		buzzer_set_tone(current_melody[0]);

		while(play_bgm){
			PORTA = 0xFF;
			for(int k=0; k<150; k++) { _delay_ms(1); ms_counter++; }
			PORTA = 0x00;
			for(int k=0; k<150; k++) { _delay_ms(1); ms_counter++; }
		}

		// 게임 종료 (무한 루프)
		while(1){
			PORTA = 0xFF;
			_delay_ms(300);
			PORTA = 0x00;
			_delay_ms(300);
		}
	}

	paused1 = 0;
	paused2 = 0;
	compare_flag = 0;
}

// ------------------------------
// Main 함수
// ------------------------------
int main(void){
	DDRC = 0xFF;     // 7-Seg Data
	DDRG |= 0x0F;    // 7-Seg Com
	DDRA = 0xFF;     // LED Score
	DDRB |= (1<<BUZZER); // PB4 출력 설정

	PORTE |= (1<<PE4)|(1<<PE5);
	EICRB = (1<<ISC41)|(1<<ISC51);
	EIMSK = (1<<INT4)|(1<<INT5);

	TCCR0 = (1<<WGM01)|(1<<CS02)|(1<<CS00);
	OCR0 = 156;
	TIMSK |= (1<<OCIE0);

	TCCR3B = 0;
	TCCR3A = 0;

	sei();

	while(1){
		if(!paused1){ num1++; if(num1>99) num1=0; }
		if(!paused2){ num2++; if(num2>99) num2=0; }

		display_4digit(num1,num2);
		display_score_led();
		soft_delay_ms(5);

		compare_and_update();
	}

	return 0;
}