/*******************************************************************************
  Reflow oven with profiles with dual switch, works on jason kits 2018 hardware.. go buy it. it's good. 
  Required Libraries
  ==================
  - Arduino PID Library:
     - Arduino PID Library:
    >> https://github.com/br3ttb/Arduino-PID-Library
  - MAX31855 Library (for board v1.60 & above):
    >> https://github.com/rocketscream/MAX31855
*******************************************************************************/
// ***** INCLUDES *****
#include <EEPROM.h>
#include <LiquidCrystal.h>
#include <MAX31855.h>
#include <PID_v1.h>

// ***** TYPE DEFINITIONS *****
typedef enum REFLOW_STATE
{
  REFLOW_STATE_IDLE,
  REFLOW_STATE_PREHEAT,
  REFLOW_STATE_SOAK,
  REFLOW_STATE_REFLOW,
  REFLOW_STATE_COOL,
s  REFLOW_STATE_COMPLETE,
  REFLOW_STATE_TOO_HOT,
  REFLOW_STATE_ERROR
} reflowState_t;

typedef enum REFLOW_STATUS
{
  REFLOW_STATUS_OFF,
  REFLOW_STATUS_ON
} reflowStatus_t;

typedef  enum SWITCH
{
  SWITCH_NONE,
  SWITCH_1,
  SWITCH_2
} switch_t;

typedef enum DEBOUNCE_STATE
{
  DEBOUNCE_STATE_IDLE,
  DEBOUNCE_STATE_CHECK,
  DEBOUNCE_STATE_RELEASE
} debounceState_t;

typedef enum REFLOW_PROFILE
{
  REFLOW_PROFILE_LEADFREE,
  REFLOW_PROFILE_LEADED,
  REFLOW_PROFILE_POWDERCOAT
} reflowProfile_t;

// ***** CONSTANTS *****
// ***** GENERAL PROFILE CONSTANTS *****
#define PROFILE_TYPE_ADDRESS 0
#define TEMPERATURE_ROOM 50
#define TEMPERATURE_SOAK_MIN 150
#define TEMPERATURE_COOL_MIN 100
#define SENSOR_SAMPLING_TIME 1000
#define SOAK_TEMPERATURE_STEP 5

// ***** LEAD FREE PROFILE CONSTANTS *****
#define TEMPERATURE_SOAK_MAX_LF 200
#define TEMPERATURE_REFLOW_MAX_LF 250
#define SOAK_MICRO_PERIOD_LF 9000

// ***** LEADED PROFILE CONSTANTS *****
#define TEMPERATURE_SOAK_MAX_PB 180
#define TEMPERATURE_REFLOW_MAX_PB 224
#define SOAK_MICRO_PERIOD_PB 10000

// ***** SWITCH SPECIFIC CONSTANTS *****
#define DEBOUNCE_PERIOD_MIN 100

// ***** PID PARAMETERS *****
// ***** PRE-HEAT STAGE *****
#define PID_KP_PREHEAT 100
#define PID_KI_PREHEAT 0.025
#define PID_KD_PREHEAT 20
// ***** SOAKING STAGE *****
#define PID_KP_SOAK 300
#define PID_KI_SOAK 0.05
#define PID_KD_SOAK 250
// ***** REFLOW STAGE *****
#define PID_KP_REFLOW 300
#define PID_KI_REFLOW 0.05
#define PID_KD_REFLOW 350
#define PID_SAMPLE_TIME 1000

// ***** LCD MESSAGES *****
const char* lcdMessagesReflowStatus[] = {
  "Ready",
  "Pre-Heat",
  "Soak",
  "Reflow",
  "Cool",
  "Done!",
  "Hot!",
  "Error!"
};

// ***** DEGREE SYMBOL FOR LCD *****
unsigned char degree[8]  = {
  140, 146, 146, 140, 128, 128, 128, 128
};

// ***** PIN ASSIGNMENT *****
//jk board 2018 pin config
int ssrPin = 2;
/*int thermocoupleSOPin = A0;
int thermocoupleCSPin = A2;
int thermocoupleCLKPin = A3;
*/
int thermocoupleSOPin = A0;
int thermocoupleCSPin = A2 ; //A2;
int thermocoupleCLKPin = A3 ; // A3
int lcdRsPin = 9;
int lcdEPin = 8;
int lcdD4Pin = 10;
int lcdD5Pin = 11;
int lcdD6Pin = 12;
int lcdD7Pin = 13;
int ledRedPin = 4;
int buzzerPin = 3;
int switchPin = A1;


// ***** PID CONTROL VARIABLES *****
double setpoint;
double input;
double output;
double kp = PID_KP_PREHEAT;
double ki = PID_KI_PREHEAT;
double kd = PID_KD_PREHEAT;
int windowSize;
unsigned long windowStartTime;
unsigned long nextCheck;
unsigned long nextRead;
unsigned long updateLcd;
unsigned long timerSoak;
unsigned long buzzerPeriod;
unsigned char soakTemperatureMax;
unsigned char reflowTemperatureMax;
unsigned long soakMicroPeriod;
// Reflow oven controller state machine state variable
reflowState_t reflowState;
// Reflow oven controller status
reflowStatus_t reflowStatus;
// Reflow profile type
reflowProfile_t reflowProfile;
// Switch debounce state machine state variable
debounceState_t debounceState;
// Switch debounce timer
long lastDebounceTime;
// Switch press status
switch_t switchStatus;
switch_t switchValue;
switch_t switchMask;
// Seconds timer
int timerSeconds;
// Thermocouple fault status
unsigned char fault;


struct PROFILE {
	char[4] name;
	int TEMPERATURE_SOAK_MAX;
	int TEMPERATURE_REFLOW_MAX;
        int SOAK_MICRO_PERIOD;
	int TEMPURATURE_SOAK_MIN;
	int SOAK_TEMPURATURE_STEP;
        int TEMPERATURE_COOL_MIN;
};

PROFILES profiles[] PROGMEM = {
	{"SnAg", 200, 250, 9000 , 150, 5, 100},
	{"SnPb", 180, 224, 10000, 150, 5, 100},
	{"Powd", 180, 224, 10000, 20, 5, 10},
};


// PID control interface
PID reflowOvenPID(&input, &output, &setpoint, kp, ki, kd, DIRECT);
// LCD interface
LiquidCrystal lcd(lcdRsPin, lcdEPin, lcdD4Pin, lcdD5Pin, lcdD6Pin, lcdD7Pin);

// MAX31855 thermocouple interface

MAX31855 thermocouple(thermocoupleSOPin, thermocoupleCSPin,
                      thermocoupleCLKPin);

void setup()
{
  // Check current selected reflow profile
  unsigned char value = EEPROM.read(PROFILE_TYPE_ADDRESS);
  if ((value == 0) || (value == 1) || (value = 2))
  {
    // Valid reflow profile value
    reflowProfile = value;
  }
  else
  {
    // Default to lead-free profile
    EEPROM.write(PROFILE_TYPE_ADDRESS, 0);
    reflowProfile = REFLOW_PROFILE_LEADFREE;
  }

  // SSR pin initialization to ensure reflow oven is off
  digitalWrite(ssrPin, LOW);
  pinMode(ssrPin, OUTPUT);

  // Buzzer pin initialization to ensure annoying buzzer is off
  digitalWrite(buzzerPin, LOW);
  pinMode(buzzerPin, OUTPUT);

  // LED pins initialization and turn on upon start-up (active LOW)
  pinMode(ledRedPin , OUTPUT);
  digitalWrite(ledRedPin , LOW);


  // Start-up splash
  digitalWrite(buzzerPin, HIGH);
  lcd.begin(16, 2);
  lcd.createChar(0, degree);
  lcd.clear();
  lcd.print("JasonKits 2018");
  lcd.setCursor(0, 1);
  lcd.print("ReflowOven V2.03");
   digitalWrite(buzzerPin, LOW);
  delay(2000);
  lcd.clear();
 
  // Serial communication at 115200 bps
  Serial.begin(57600);

  // Turn off LED (active LOW)
  digitalWrite(ledRedPin , HIGH);
  // Set window size
  windowSize = 2000;
  // Initialize time keeping variable
  nextCheck = millis();
  // Initialize thermocouple reading variable
  nextRead = millis();
  // Initialize LCD update timer
  updateLcd = millis();
}

void loop()
{
  // Current time
  unsigned long now;

  // Time to read thermocouple?
  if (millis() > nextRead)
  {
    // Read thermocouple next sampling period
    nextRead += SENSOR_SAMPLING_TIME;
    // Read current temperature

    input = thermocouple.readThermocouple(CELSIUS);

    // If thermocouple problem detected

    if ((input == FAULT_OPEN) || (input == FAULT_SHORT_GND) ||
        (input == FAULT_SHORT_VCC))

    {
      // Illegal operation
      reflowState = REFLOW_STATE_ERROR;
      reflowStatus = REFLOW_STATUS_OFF;
    }
  }


  if (millis() > nextCheck)
  {
    // Check input in the next seconds
    nextCheck += 1000;
    // If reflow process is on going
    if (reflowStatus == REFLOW_STATUS_ON)
    {
      // Toggle red LED as system heart beat
      digitalWrite(ledRedPin , !(digitalRead(ledRedPin )));
      // Increase seconds timer for reflow curve analysis
      timerSeconds++;
      // Send temperature and time stamp to serial
      Serial.print(timerSeconds);
      Serial.print(",");
      Serial.print(setpoint);
      Serial.print(",");
      Serial.print(input);
      Serial.print(",");
      Serial.println(output);
    }
    else
    {
      // Turn off red LED
      digitalWrite(ledRedPin , HIGH);
    }
  }

  if (millis() > updateLcd)
  {
    // Update LCD in the next 250 ms
    updateLcd += 250;
    // Clear LCD
    lcd.clear();
    
    // Print current system state on LCD 
    lcd.print(lcdMessagesReflowStatus[reflowState]);

     
    // show the setpoint temperature
    lcd.setCursor (8,0);
    lcd.print(setpoint); //setpoint     
//write the degrees 
#if ARDUINO >= 100
      // Display degree Celsius symbol
      lcd.write((uint8_t)0);
#else
      // Display degree Celsius symbol
      lcd.print(0, BYTE);
#endif
      lcd.print("C ");


    // show the timer 
    lcd.setCursor(8,1);
    lcd.print(timerSeconds);
    lcd.print("s "); 
    

    lcd.setCursor(12, 1);
    if (reflowProfile == REFLOW_PROFILE_LEADFREE)
    {
      lcd.print("SnAg");
    }
    else
    {
      lcd.print("SnPb");
    }
    // Move the cursor to the 2 line
    lcd.setCursor(0, 1);

    // If currently in error state
    if (reflowState == REFLOW_STATE_ERROR)
    {
      // Thermocouple error (open, shorted)
      lcd.print("TC Error");
    }
    else
    {
      // Display current temperature
      lcd.print(input);

#if ARDUINO >= 100
      // Display degree Celsius symbol
      lcd.write((uint8_t)0);
#else
      // Display degree Celsius symbol
      lcd.print(0, BYTE);
#endif
      lcd.print("C ");
    }
  }

  // Reflow oven controller state machine
  switch (reflowState)
  {
    case REFLOW_STATE_IDLE:
      // If oven temperature is still above room temperature
      if (input >= TEMPERATURE_ROOM)
      {
        reflowState = REFLOW_STATE_TOO_HOT;
      }
      else
      {
        // If switch is pressed to start reflow process
        if (switchStatus == SWITCH_1)
        {
          // Send header for CSV file
          Serial.println("Time,Setpoint,Input,Output");
          // Intialize seconds timer for serial debug information
          timerSeconds = 0;
          // Initialize PID control window starting time
          windowStartTime = millis();
          // Ramp up to minimum soaking temperature
          setpoint = profiles[reflowProfile].TEMPERATURE_SOAK_MIN;
	  soakTemperatureMax = profiles[reflowProfile].TEMPERATURE_SOAK_MAX;
	  reflowTemperatureMax =  profiles[reflowProfile].TEMPERATURE_REFLOW_MAX;
          soakMicroPeriod =  profiles[reflowProfile].SOAK_MICRO_PERIOD;
          // Tell the PID to range between 0 and the full window size
          reflowOvenPID.SetOutputLimits(0, windowSize);
          reflowOvenPID.SetSampleTime(PID_SAMPLE_TIME);
          // Turn the PID on
          reflowOvenPID.SetMode(AUTOMATIC);
          // Proceed to preheat stage
          reflowState = REFLOW_STATE_PREHEAT;
        }
      }
      break;

    case REFLOW_STATE_PREHEAT:
      reflowStatus = REFLOW_STATUS_ON;
      // If minimum soak temperature is achieve
      if (input >= TEMPERATURE_SOAK_MIN)
      {
        // Chop soaking period into smaller sub-period
        timerSoak = millis() + soakMicroPeriod;
        // Set less agressive PID parameters for soaking ramp
        reflowOvenPID.SetTunings(PID_KP_SOAK, PID_KI_SOAK, PID_KD_SOAK);
        // Ramp up to first section of soaking temperature
        setpoint = TEMPERATURE_SOAK_MIN + SOAK_TEMPERATURE_STEP;
        // Proceed to soaking state
        reflowState = REFLOW_STATE_SOAK;
      }
      break;

    case REFLOW_STATE_SOAK:
      // If micro soak temperature is achieved
      if (millis() > timerSoak)
      {
        timerSoak = millis() + soakMicroPeriod;
        // Increment micro setpoint
        setpoint += SOAK_TEMPERATURE_STEP;
        if (setpoint > soakTemperatureMax)
        {
          // Set agressive PID parameters for reflow ramp
          reflowOvenPID.SetTunings(PID_KP_REFLOW, PID_KI_REFLOW, PID_KD_REFLOW);
          // Ramp up to first section of soaking temperature
          setpoint = reflowTemperatureMax;
          // Proceed to reflowing state
          reflowState = REFLOW_STATE_REFLOW;
        }
      }
      break;

    case REFLOW_STATE_REFLOW:
      // We need to avoid hovering at peak temperature for too long
      // Crude method that works like a charm and safe for the components
      if (input >= (reflowTemperatureMax - 5))
      {
        // Set PID parameters for cooling ramp
        reflowOvenPID.SetTunings(PID_KP_REFLOW, PID_KI_REFLOW, PID_KD_REFLOW);
        // Ramp down to minimum cooling temperature
        setpoint = TEMPERATURE_COOL_MIN;
        // Proceed to cooling state
        reflowState = REFLOW_STATE_COOL;
      }
      break;

    case REFLOW_STATE_COOL:
      // If minimum cool temperature is achieve
      if (input <= TEMPERATURE_COOL_MIN)
      {
        // Retrieve current time for buzzer usage
        buzzerPeriod = millis() + 1000;
        // Turn on buzzer to indicate completion
        digitalWrite(buzzerPin, HIGH);
        // Turn off reflow process
        reflowStatus = REFLOW_STATUS_OFF;
        // Proceed to reflow Completion state
        reflowState = REFLOW_STATE_COMPLETE;
      }
      break;

    case REFLOW_STATE_COMPLETE:
      if (millis() > buzzerPeriod)
      {
        // Turn off buzzer
        digitalWrite(buzzerPin, LOW);
        // Reflow process ended
        reflowState = REFLOW_STATE_IDLE;
      }
      break;

    case REFLOW_STATE_TOO_HOT:
      // If oven temperature drops below room temperature
      if (input < TEMPERATURE_ROOM)
      {
        // Ready to reflow
        reflowState = REFLOW_STATE_IDLE;
      }
      break;

    case REFLOW_STATE_ERROR:
      // If thermocouple problem is still present
      /*  #ifdef  USE_MAX6675
          if (isnan(input))
        #else
      */
      if ((input == FAULT_OPEN) || (input == FAULT_SHORT_GND) ||
          (input == FAULT_SHORT_VCC))
        // #endif
      {
        // Wait until thermocouple wire is connected
        reflowState = REFLOW_STATE_ERROR;
      }
      else
      {
        // Clear to perform reflow process
        reflowState = REFLOW_STATE_IDLE;
      }
      break;
  }

  // If switch 1 is pressed
  if (switchStatus == SWITCH_1)
  {
    // If currently reflow process is on going
    if (reflowStatus == REFLOW_STATUS_ON)
    {
      // Button press is for cancelling
      // Turn off reflow process
      reflowStatus = REFLOW_STATUS_OFF;
      // Reinitialize state machine
      reflowState = REFLOW_STATE_IDLE;
    }
  }
  // Switch 2 is pressed
  else if (switchStatus == SWITCH_2)
  {
    // Only can switch reflow profile during idle
    if (reflowState == REFLOW_STATE_IDLE)
    {
	//move to the next profile..
	reflowProfile++;
      // if does not exist, goto the first of the list.
      if (reflowProfile == sizeof(profiles)) 
      {
	reflowProfile = 0;
      }
      EEPROM.write(PROFILE_TYPE_ADDRESS, 1);
    }
  }
  // Switch status has been read
  switchStatus = SWITCH_NONE;

  // Simple switch debounce state machine (analog switch)
  switch (debounceState)
  {
    case DEBOUNCE_STATE_IDLE:
      // No valid switch press
      switchStatus = SWITCH_NONE;

      switchValue = readSwitch();

      // If either switch is pressed
      if (switchValue != SWITCH_NONE)
      {
        // Keep track of the pressed switch
        switchMask = switchValue;
        // Intialize debounce counter
        lastDebounceTime = millis();
        // Proceed to check validity of button press
        debounceState = DEBOUNCE_STATE_CHECK;
      }
      break;

    case DEBOUNCE_STATE_CHECK:
      switchValue = readSwitch();
      if (switchValue == switchMask)
      {
        // If minimum debounce period is completed
        if ((millis() - lastDebounceTime) > DEBOUNCE_PERIOD_MIN)
        {
          // Valid switch press
          switchStatus = switchMask;
          // Proceed to wait for button release
          debounceState = DEBOUNCE_STATE_RELEASE;
        }
      }
      // False trigger
      else
      {
        // Reinitialize button debounce state machine
        debounceState = DEBOUNCE_STATE_IDLE;
      }
      break;

    case DEBOUNCE_STATE_RELEASE:
      switchValue = readSwitch();
      if (switchValue == SWITCH_NONE)
      {
        // Reinitialize button debounce state machine
        debounceState = DEBOUNCE_STATE_IDLE;
      }
      break;
  }

  // PID computation and SSR control
  if (reflowStatus == REFLOW_STATUS_ON)
  {
    now = millis();

    reflowOvenPID.Compute();

    if ((now - windowStartTime) > windowSize)
    {
      // Time to shift the Relay Window
      windowStartTime += windowSize;
    }
    if (output > (now - windowStartTime)) digitalWrite(ssrPin, HIGH);
    else digitalWrite(ssrPin, LOW);
  }
  // Reflow oven process is off, ensure oven is off
  else
  {
    digitalWrite(ssrPin, LOW);
  }
}

switch_t readSwitch(void)
{
  int switchAdcValue = 0;

  switchAdcValue = analogRead(switchPin);

  // Add some allowance (+10 ADC step) as ADC reading might be off a little
  // due to 3V3 deviation and also resistor value tolerance
  if (switchAdcValue >= 1000) return SWITCH_NONE;
  if (switchAdcValue <= 10) return SWITCH_1;
  if (switchAdcValue <= 522) return SWITCH_2;

  return SWITCH_NONE;
}
