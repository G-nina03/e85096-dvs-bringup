// Linux Kernel Driver

/* 현재 구현 사항
 * 1. DVS 센서의 Reset 제어 
 * 2. I2C 통신을 통해 레지스터에 값 쓰고 읽기 & 통신이 정상인지 확인
 */

/* 구현이 필요한 사항
 * 1. MIPI CSI2 data 수신
 * 2. DMA, DVS 이벤트 디코딩
 */


//////////////////////////////////////////////////////////////////////////////////////////////////////////
//**************************Linux Kernel API 사용을 위한 header file 정의********************************
//////////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h> // I2C client, message, Transfer
#include <linux/jiffies.h> // 시간 단위를 kernel tick으로 변환
#include <linux/module.h> // kernel module 등록 및 정보
#include <linux/mutex.h> // mutex를 이용한 동기화
#include <linux/of.h> //device tree기반 장치 매칭
#include <linux/slab.h> //kernel memory 할당
#include <linux/workqueue.h> // wldus wkrdjq cjfl 

#define DVS_RETRY_INITIAL_MS 2000U //재시도 초기 대기 시간 (2000ms)
#define DVS_RETRY_MAX_MS 30000U //재시도 최대 대기 시간, 3000ms



////////////////////////////////////////////////////////////////////////////////////////////////////////
//*********************** I2C 주소와 메시지 생성 Macro *************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

// makes i2c address
#define MAKE_I2C_ADDR(addr) {(addr) >> 8, (addr) & 0xff} // verilog의 concatanation이 아닌 C의 배열 분리 연산자. -> DVS reg addr의  상위 8비트와 하위 8비트를 나누어 저장.

#define I2C_READ(dev_addr, reg_addr, dst, count)                               \ // I2C read 매크로 정의. Slave address, Register Address, 수신 버퍼, 읽을 크기를 인자로 받음.
  {.addr = (dev_addr), .flags = !I2C_M_RD, .len = 2, .buf = (reg_addr)}, {     \ // 첫번째 I2C Message 생성. 인자로 받은 I2C의 주소로 인자로 받은 DVS reg addr 2바이트를 write
    .addr = (dev_addr), .flags = I2C_M_RD, .len = (count), .buf = (dst)        \ // 두번째 I2C Message 생성. 지정한 cout 크기 만큼 read 하여 dst에 저장.
  }

#define I2C_WRITE(dev_addr, reg_addr, ...)                                     \ // I2C write 매크로 정의. '...'은 가변 인자. 이 매크로에서는 write할 데이터.
  {.addr = (dev_addr),                                                         \ // 인자로 받은 I2C Addr 주소를 통해
   .flags = !I2C_M_RD,                                                         \ // write 할거임
   .len = sizeof((u8[]){(reg_addr)[0], (reg_addr)[1], __VA_ARGS__}),           \ // 인자로 받은 register 2바이트와 data(__VA_ARGS__: ... 가변인자)의 전체 크기를 계산
   .buf = ((u8[]){(reg_addr)[0], (reg_addr)[1], __VA_ARGS__})}    		 // 실제 전송할 Byte Array를 생성하여 buffer에 저장.

#define DVS_ADDR_PAD_CONTROL 0x303B // write/read back 테스트 대상이 되는 register 지정. 




////////////////////////////////////////////////////////////////////////////////////////////////////////
//********************** "구조체 정의"driver 상태 및 자원 관리 ******************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

struct dvs_data { // DVS 드라이버의 실행 상태와 하드웨어 제어에 필요한 정보를 하나의 구조체에 저장.
  struct i2c_client *client; // Linux kernel에서 특정 I2C slave device를 표현하는 구조체. 
			     // client->addr: 현재 DVS에서는 0x10
			     // client->adapter: I2C Bus controller에 대응하는 Linux adaptor (I2C 통신을 수행하는 interface)
			     // dev: linux device module의 장치 객체.
  struct gpio_desc *reset_gpio; // reset 핀 제어용 GPIO Descriptor
  struct delayed_work check_work; // 일정 시간 후 실행할 검사 작업

  struct mutex lock; 
  unsigned int retry_delay_ms; //현재 재시도 대기 시간
  bool stopping; // 드라이버가 종료 중인지 나타냄 (=true: 종료 절차 진입)
  // This represents software standby state
  bool ready; //I2C 검증 성공 상태 확인 (=true: 성공)
};



////////////////////////////////////////////////////////////////////////////////////////////////////////
//************************ 4. RESET, I2C R/W. 재시도 ******************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

static void dvs_check_work(struct work_struct *work) { //dvs_check_work 함수 정의; workqueue가 실행하는 callback,
  struct dvs_data *dvs = //앞서 생성한 구조체 활용; dvs라는 구조체 포인터 선언 동
			 //현재 구조: dvs 포인터가 dvs_data 구조체를 가리키며, 내부의 delayed_work 구조체가 check_work로 선언되어 있음.
			 //workqueue가 callback을 실행할 때는 work_Struct *만 전달하므로, 해당 work가 어떤 dvs 장치에 속하는지 알아야 함.
			 //container_of는 구조체 멤버의 주소를 이용하여 그 멤버를 포함하는 상위 구조체의 주고를 구하는 매크로.
			 //to_delayed_work(work)는 work_struct에서 delayed_work를 얻고, container_of는 그 delayed_work가 포함된 dvs_data를 얻음.
			 //이후 모든 DVS 상태 변수에 dvs-> 로 접근 가능.
      container_of(to_delayed_work(work), struct dvs_data, check_work); // 전달 받은 work 포인터에서 원래의 dvs_data 구조체 주소 계산

  mutex_lock(&dvs->lock); // 해당 구조체 mutex 획득 

  if (dvs->stopping) //현재 dvs에 해당하는 드라이버가 종료 중인지 검사
    goto out; // 종료 중이라면 out label로 이동.


  // 종료 중이 아니라면 ,reset 시작.
  // Drive RSTN high before asserting reset.
  gpiod_set_value_cansleep(dvs->reset_gpio, 0); //GPIO 논리적 활성 상태 0(비활성), 물리적 high
  // Assert reset by driving the active-low RSTN pin low.
  gpiod_set_value_cansleep(dvs->reset_gpio, 1); //GPIO 논리적 활성 상태 1(활성), 물리적 low (active-low)
  // Hold RESET for 100 us during this bring-up test.
  udelay(100);
  // Release reset by driving RSTN high to enter software standby.
  gpiod_set_value_cansleep(dvs->reset_gpio, 0);
  // Wait at least t2 (10 us) before the first I2C transaction.
  udelay(10); //첫번째 I2C transaction 전 최소 10us 대기.언
	      

  //test data 준비
  static u8 initial_value = 0xAA; /* 0b10101010 */  //write 초기값 설정 
  u8 readback_value = -1; //read 값을 저장할 8bit 변수 생성

  u8 address[] = MAKE_I2C_ADDR(DVS_ADDR_PAD_CONTROL); //앞서 지정한 test 용 DVS reg 주소를 2byte 배열로 변환
  int ret = 0; //I2C 함수 반환값을 저장할 변수 선언
	       

  // I2C register write
  struct i2c_msg write_reg_msg[] = {
      I2C_WRITE(dvs->client->addr, address, initial_value)}; // I2C addr, DVS reg addr, wrtie data
  ret = i2c_transfer(dvs->client->adapter, write_reg_msg, // i2c_transfer(adaptor, msgs, num); adaptor사용할 I2C controller, msgs전공할 I2C massage배열, num배열에 포함된 msg 수
                     ARRAY_SIZE(write_reg_msg)); // 성공 시 처리된 message 개수를 반환. 현재는 1개 이므로 ret==1 예상.
  if (ret != ARRAY_SIZE(write_reg_msg)) { //실제 처리된 메시지 수가 기대한 수와 다른지 검사
    dev_err(&dvs->client->dev, "Failed to send i2c\n"); //실패 시 kernel error log 출력
    goto retry; // & retry label로 이동, read는 실행 x
  }

  //I2C register read
  struct i2c_msg readback_reg_msg[] = {I2C_READ(
      dvs->client->addr, address, &readback_value, sizeof(readback_value))}; //I2C addr, DVS reg addr, 수신 buffer 주소, data 크기 
  ret = i2c_transfer(dvs->client->adapter, readback_reg_msg, // reg 주소 write, 읽어온 값을 buffer에 저장. 두 message 처리. 성공했다면 ret==2 예상.
                     ARRAY_SIZE(readback_reg_msg));
  if (ret != ARRAY_SIZE(readback_reg_msg)) {
    dev_err(&dvs->client->dev, "Failed to recv i2c\n");
    goto retry;
  }


  // 데이터 비교 및 재시도 
  dev_dbg(&dvs->client->dev, "I2C readback: wrote=0x%02x read=0x%02x\n",
          initial_value, readback_value);
  if (initial_value != readback_value) { // write 값과 read back 값이 다르면 실패로 판단, retry.
  retry:
    dev_dbg(&dvs->client->dev, //retry 예정 시간을 debug log 로 출력,
            "DVS not ready or register corrupted; retry in %u ms\n",
            dvs->retry_delay_ms);
    // Keep RSTN low until the next attempt.
    gpiod_set_value_cansleep(dvs->reset_gpio, 1); // 다음 시도까지는 reset 상태를 ㅇ휴지
    /* Retry after 2, 4, 8, 16, 30, 30... seconds; stop after success. */
    schedule_delayed_work(&dvs->check_work, // delayed work를 다시 예약
                          msecs_to_jiffies(dvs->retry_delay_ms)); // 현재 지연 시간을 kernel tick으로 변환
    dvs->retry_delay_ms = min(dvs->retry_delay_ms * 2U, DVS_RETRY_MAX_MS); //다음 재시도 간격을 두배로 증가, 최대 30초
    initial_value = ~initial_value;
  } else { //test 성공 시 
    dvs->ready = true;
    dvs->retry_delay_ms = DVS_RETRY_INITIAL_MS; // 재시도 간격은 최소인 2ms로 초기화
    dev_info(&dvs->client->dev,
             "DVS ready: I2C response verified (0x303B=0x%02x)\n", // I2C 검증 성공 메시지 출력.
             readback_value);
  }

out:
  mutex_unlock(&dvs->lock); // work 종료, mutex 해제. 
}




////////////////////////////////////////////////////////////////////////////////////////////////////////
//*************************** 3. 장치 인식 및 초기화 ******************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

//Prove 함수: Linux kernel이 I2C device와 driver가 매칭된 것을 확인하면 호출하는 초기화 함수.
//device가 등록되고 해당 장치와 매칭되면 dvs_prove()가 호출될 수 있음.
//앞서 device tree에서 compatible = "nvr,rc1s"; 정의. & 코드 후반부에는 같은 문자열을 가진 of_device_id가 존재.

static int dvs_probe(struct i2c_client *client) { //dvs_probe 함수 정의
  struct dvs_data *dvs; //dvs 드라이버 상태를 저장할 포인터 선언

  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) //I2C adaptopr가 일반 I2C transfer를 지원하는지 검사
    return dev_err_probe(&client->dev, -EOPNOTSUPP,
                         "Adapter does not support plain I2C transfers\n");

  dvs = devm_kzalloc(&client->dev, sizeof(*dvs), GFP_KERNEL); //dvs 구조체에 필요한 kernel 메모리 할당
  if (!dvs) { //메모리 할당 실패 여부 검사
    return -ENOMEM;
  }

  dvs->client = client; //I2C client 포인터를 dvs 구조체에 저장
  mutex_init(&dvs->lock); // mutex 초기화
  // This would set DVS_Reset pin to HIGH (which is inactive, To reset DVS
  // Device, RSTN is required to go LOW for 0.1us)
  dvs->reset_gpio = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_LOW); // GPIO 요청, output 설정, 초기 논리값 0 지정. 

  if (IS_ERR(dvs->reset_gpio)) { //GPIO 획득 실패 여부 검사 
    return dev_err_probe(&client->dev, PTR_ERR(dvs->reset_gpio),
                         "Failed to acquire reset GPIO\n");
  }

  INIT_DELAYED_WORK(&dvs->check_work, dvs_check_work); // delayed work 초기화 및 callback 연결 
  
  /* Associate the driver state with this I2C client. */
  i2c_set_clientdata(client, dvs); // I2C client에 dvs 포인터 저장 
  dvs->retry_delay_ms = DVS_RETRY_INITIAL_MS; //재시도 간격을 초기 2초로 설정 

  mutex_lock(&dvs->lock); // Mutex 획득 
  dvs->stopping = false; // 종료 상태 false 지정
  dvs->ready = false; // 준비 상태 fault 지정

  dev_info(&client->dev, "DVS driver loaded\n"); // device driver 초기화 성공 시 'DVS driver loaded' 메시지 출력

  // Check the device immediately, then retry with exponential backoff.
  schedule_delayed_work(&dvs->check_work, 0); // 지연 시간 0으로 설정 후 즉시 장치 검사, 실패 시 backoff 재시도.
  mutex_unlock(&dvs->lock); // mutex 해제 
  return 0; // Linux driver 초기화 성공 시 반환 
}



////////////////////////////////////////////////////////////////////////////////////////////////////////
//******************************* 6. 작업 중단 ************************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

static void dvs_stop_check(struct i2c_client *client) {
  struct dvs_data *dvs = i2c_get_clientdata(client); // I2C client에서 dvs_data 획득

  mutex_lock(&dvs->lock); // Mutex 획득
  dvs->stopping = true; // dvs 종료 활성화
  dvs->ready = false; // 준비 상태 비활성화
  mutex_unlock(&dvs->lock); //Mutex 해제

  /* Never wait for the worker while holding its mutex. */
  cancel_delayed_work_sync(&dvs->check_work); // dvs 설정값 설정에만 mutex를 획득하고 실제 worker 종료 기간 동안에는 mutex를 반환한 채로 진행
} //BC. 위 종료 함수도 mutex를 사용하기 때문.



////////////////////////////////////////////////////////////////////////////////////////////////////////
//********************************** 5. 드라이버 해제 *****************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

static void dvs_remove(struct i2c_client *client) {
  struct dvs_data *dvs = i2c_get_clientdata(client);
  dvs_stop_check(client); // 드라이버 해제 시 우선 실행 중인 검사 작업 종료

  /* Assert RESET after stopping the I2C worker. */
  gpiod_set_value_cansleep(dvs->reset_gpio, 1); // worker 종료 이후 reset 활성화

  dev_info(&client->dev, "DVS driver removed\n"); // remove 확인 메세지 출
}



////////////////////////////////////////////////////////////////////////////////////////////////////////
//********************************* 2. Device Tree 매칭 ***************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

/* Must match the compatible string in the Device Tree. */
static const struct of_device_id dvs_of_match[] = {{.compatible = "nrv,rc1s"},
                                                   {}};
MODULE_DEVICE_TABLE(of, dvs_of_match);




////////////////////////////////////////////////////////////////////////////////////////////////////////
//********************************** 1. 드라이버 등록 *****************************************************
////////////////////////////////////////////////////////////////////////////////////////////////////////

static struct i2c_driver dvs_driver = { // Linux I2C driver 구조체 선언
    .driver = //driver 구조체 시작
        {
            .name = "dvs", //driver 이름을 dvs로 지정
            .of_match_table = dvs_of_match, // Device Tree Match Table 연결
        },
    .probe = dvs_probe, // Probe callback 등록
    .remove = dvs_remove, // Remove callback 등록
    .shutdown = dvs_stop_check, //Shutdown callback 등록
};

module_i2c_driver(dvs_driver); //I2C Driver를 Module로 등록하는 매크로 
// 모듈 초기화 시 I2C driver를 등록, 종료 시 등록을 해제하는 코드 생성.
// -> 사용자가 직접 module_init()과 module exit()을 작성하지 않아도 됨.


////////////////////////////////////////////////////////////////////////////////////////////////////////

MODULE_AUTHOR("Kim Minsu <happytanuki@nrv.com>");
MODULE_DESCRIPTION("RC1S DVS driver");
MODULE_LICENSE("GPL");
