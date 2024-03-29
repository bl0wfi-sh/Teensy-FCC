
#include "BMI088.h"
#include "Servo.h"      // Included this so we can run Servo's @ 400Hz. Have not verified they actually run @ 400Hz.
#include "sbus.h"       // For reading in sbus information. 
#include "Kalman.h"
#include "Complimentary.h"
#include "PIDController.h"
#include "MFController.h"

//*********** Motor Parameters and Variables
#define FLM   3
#define FRM   4
#define BRM   5
#define BLM   6
#define MIN_MOTOR_MS_VAL 1080       // At this value motors start spinning!
#define MAX_MOTOR_MS_VAL 1950
Servo FrontLeft, FrontRight, BackLeft, BackRight;

//*********** RC receiver variables.
bfs::SbusRx sbus_rx(&Serial1);
std::array<int16_t, bfs::SbusRx::NUM_CH()> sbus_data;
#define MAX_CH_VAL  1693
#define MIN_CH_VAL  304
#define MID_CH_VAL  1387
#define ROLL_CH     1
#define PITCH_CH    2
#define THROTTLE_CH 3
#define YAW_CH      4
#define MODE_CH     5
#define ARM_CH      10
int throttle;
float des_pitch;
float des_roll;
float des_yaw;
float throttle_queue[10] = (0,0,0,0,0,0,0,0,0,0};
float roll_queue[10] = (0,0,0,0,0,0,0,0,0,0};
float pitch_queue[10] = (0,0,0,0,0,0,0,0,0,0};
float yaw_queue[10] = (0,0,0,0,0,0,0,0,0,0};
float* rolling_average_rc(float new_throttle, float new_roll, float new_pitch, float new_yaw)
{
  float results[4] = {new_throttle, new_roll, new_pitch, new_yaw}; // throttle, roll, pitch, yaw
  
  // Shift things up
  for(int i = 9; i > 0; i--)
  {
    throttle_queue[i] = throttle_queue[i-1];
    results[0] += throttle_queue[i];
    
    roll_queue[i] = roll_queue[i-1];
    results[1] += roll_queue[i];
    
    pitch_queue[i] = pitch_queue[i-1];
    results[2] += pitch_queue[i];
    
    yaw_queue[i] = yaw_queue[i-1];
    results[3] += yaw_queue[i];
  }

  // Update rolling queue with new values.
  throttle_queue[0] = new_throttle;
  roll_queue[0] = new_roll;
  pitch_queue[0] = new_pitch;
  yaw_queue[0] = new_yaw;

  // Finalize average and return array.
  for(int i = 0; i < 4; i++)
  {
    results[i] /= 10.0;
  }

  return results;
}

//*********** Modes
// 0 - Off mode. LED blinks slow.
// 1 - Acro Mode (No attitude control, just rate control. BE AWARE STICKS COMMAND RATES NOT DEGREES!!!)
// 2 - Stablized Mode (Attitude controller cascades into rate controller! STICKS COMMAND DEGREES!!!). LED blinks fast!
int flight_mode;
bool state_updated;
bool armed;
const int ledpin = LED_BUILTIN;
int ledstate = LOW;

IntervalTimer modeTimer;
void blinkLED(){              // Mode timer callback function.
  if (ledstate == LOW) {
    ledstate = HIGH;
  } else {
    ledstate = LOW;
  }
  digitalWrite(ledpin, ledstate);
}

//*********** Raw IMU variables
float ax = 0, ay = 0, az = 0;
float gx = 0, gy = 0, gz = 0;
int16_t temp = 0;

//*********** State Estimation
float veh_yaw;

Kalman k_roll_estimator;
Kalman k_pitch_estimator;
float k_roll = 0, k_pitch = 0;

Complimentary c_roll_estimator;
Complimentary c_pitch_estimator;
float c_roll = 0, c_pitch = 0;
#define ESTIMATION_TIME   .002          // In seconds!
IntervalTimer stateEstimator;           // Estimator will run @ 800Hz.
void updateEstimators(){
  // Read accelerometer
  //temp = bmi088.getTemperature();
  bmi088.getAcceleration(&ax, &ay, &az);  // g/s^2
  bmi088.getGyroscope(&gx, &gy, &gz);     // deg/s

  // Estimation of yaw. I don't know any other way to do this with the current sensors.
  veh_yaw += (-gz * ESTIMATION_TIME);

  // Estimating roll and pitch angle based on accelerometer.
  float acc_tot_vect = sqrt((ax*ax) + (ay*ay) + (az*az));
  float acc_roll = asin(ay/acc_tot_vect) * (180.0 /3.142);
  float acc_pitch = asin(ax/acc_tot_vect) * (180.0 /3.142);

  // Lets use the complimentary filter to estimate roll and pitch.
  c_roll = c_roll_estimator.updateAngle(acc_roll, gx, ESTIMATION_TIME);
  c_pitch = c_pitch_estimator.updateAngle(acc_pitch, -gy, ESTIMATION_TIME);
  //gyro_roll += gyro_pitch * sin(-gz * ESTIMATION_TIME * 3.142 / 180.0);
  //gyro_pitch -= gyro_roll * sin(-gz * ESTIMATION_TIME * 3.142 / 180.0);
    
  // Lets use the kalman filter to estimate roll and pitch.
  k_roll = k_roll_estimator.getAngle(acc_roll, gx, ESTIMATION_TIME);
  k_pitch = k_pitch_estimator.getAngle(acc_pitch, -gy, ESTIMATION_TIME);
}

//*********** Vehicle Control Algorithms
#define LOOP_TIME .0025   // In seconds!
#define MAX_RATE_COMMAND 400.0           // Max allowable rate command from attitude controller to rate controller. deg/s
#define MIN_RATE_COMMAND -400.0          // Min allowable rate command from attitude controller to rate controller. deg/s
std::array<float, 3> attitude_roll_gains =  {2.4, 0.0, 0.0};        // PID - Roll
std::array<float, 3> attitude_pitch_gains = {2.4, 0.0, 0.0};        // PID - Pitch
std::array<float, 3> attitude_yaw_gains =   {1.2, 0.0, 0.0};        // PID - Yaw
PIDControl attitude_controller(attitude_roll_gains, attitude_pitch_gains, attitude_yaw_gains);

std::array<float, 3> rate_roll_gains =      {1.3, 0.01, 0.0005};    // PID - Roll
std::array<float, 3> rate_pitch_gains =     {1.3, 0.01, 0.0005};    // PID - Pitch
std::array<float, 3> rate_yaw_gains =       {3.0, 0.07, 0.0};       // PID - Yaw
PIDControl rate_controller(rate_roll_gains, rate_pitch_gains, rate_yaw_gains);

std::array<float, 4> roll_params =      {0.00001, 0.000, 1.0, 0.01};    // Intelligent PD Controller - Roll
std::array<float, 4> pitch_params =     {0.00001, 0.000, 1.0, 0.01};    // Intelligent PD Controller - Pitch
std::array<float, 4> yaw_params =       {0.00001, 0.000, 1.0, 0.01};    // Intelligent PD Controller - Yaw
MFControl mf_controller(roll_params, pitch_params, yaw_params);

float fl = 0, fr = 0, bl = 0, br = 0;

//*********** Debug console!
#define LOGGER_TIME .01   // In seconds!
IntervalTimer logTimer;
void logToConsole(){
  /** Throttle debugging    // Logger timer callback function.
  Serial.print(throttle);
  Serial.println();
  **/

  /** Debug estimators.
  Serial.print(k_roll);
  Serial.print(", ");
  Serial.print(c_roll);
  Serial.print(", ");
  Serial.print(k_pitch);
  Serial.print(", ");
  Serial.print(c_pitch);
  Serial.print(", ");
  Serial.print(veh_yaw);
  Serial.print(", ");
  **/
  Serial.print(fl);
  Serial.print(", ");
  Serial.print(fr);
  Serial.print(", ");
  Serial.print(bl);
  Serial.print(", ");
  Serial.println(br);

  /** Debug Controllers
  Serial.print(des_roll);
  Serial.print(", ");
  Serial.print(des_pitch);
  Serial.print(", ");
  Serial.print(des_yaw);
  Serial.print(", ");
  Serial.print(roll_err);
  Serial.print(", ");
  Serial.print(pitch_err);
  Serial.print(", ");
  Serial.print(yaw_err);
  Serial.print(", ");
  Serial.print(comm_roll);
  Serial.print(", ");
  Serial.print(comm_pitch);
  Serial.print(", ");
  Serial.println(comm_yaw);
  **/
  
  /** Log for telemetry visualization
  Data structure:
  gx, gy, gz, ax, ay, az, att_errors(r,p,y), att_setpoint(r,p,y), rate_errors(r,p,y), rate_setpoint(r,p,y), throttle
  **/
}

void setup(void) {
  // Starting communication buses.
  Wire.begin();
  Wire.setClock(400000UL);         // Set I2C frequency to 400kHz.
  Serial.begin(1000000);

  // Set default operational mode to manual
  flight_mode = 2;
  armed = false;          // Default not armed.
  pinMode(ledpin, OUTPUT);
  modeTimer.begin(blinkLED, 500000);

  // Start logger process only if usb cable is connected.
  // When USB is not connected not having this running will help save cycles.
  if (Serial)
  {
    logTimer.begin(logToConsole, (int)(LOGGER_TIME *1000000));      // Logging @ 100Hz
  }
    
  // Setup sbus receiver.
  sbus_rx.Begin();

  // Setup motor PWM pins as outputs.
  FrontLeft.attach(FLM, 1000, 2000);
  FrontRight.attach(FRM, 1000, 2000);
  BackLeft.attach(BLM, 1000, 2000);
  BackRight.attach(BRM, 1000, 2000);

  // Sets ESC's to 0 throttle.
  FrontLeft.writeMicroseconds(1000);
  FrontRight.writeMicroseconds(1000);
  BackLeft.writeMicroseconds(1000);
  BackRight.writeMicroseconds(1000);
  
  // Waiting to see if IMU is connected.
  while (1) {
      if (bmi088.isConnection()) {
          bmi088.initialize();
          break;
      } else {
          Serial.println("BMI088 is not connected");
      }
      delay(1);
  }

  delay(100);

  // Initialize gyro angles to accelerometer readings in the beginning.
  float avg_ax = 0;
  float avg_ay = 0;
  float avg_az = 0;
  for(int i = 0; i < 100; i++)
  {
      bmi088.getAcceleration(&ax, &ay, &az);  // g/s^2 
      avg_ax += ax;
      avg_ay += ay;
      avg_az += az;
      delay(1);
  }
  avg_ax = avg_ax / 100.0;
  avg_ay = avg_ay / 100.0;
  avg_az = avg_az / 100.0;
  
  float acc_tot_vect = sqrt((avg_ax*avg_ax) + (avg_ay*avg_ay) + (avg_az*avg_az));
  float acc_roll = asin(avg_ay/acc_tot_vect) * (180.0 /3.142);
  float acc_pitch = asin(avg_ax/acc_tot_vect) * (180.0 /3.142);

  // Initialize the complimentary filter.
  c_roll_estimator.setAngle(acc_roll);    // Setting initial angle to accelerometer value.
  c_pitch_estimator.setAngle(acc_pitch);
  c_roll_estimator.setTao(0.45);           // Setting low and high pass filter time constant.
  c_pitch_estimator.setTao(0.45);

  // Initialize the kalman filter.
  k_roll_estimator.setAngle(acc_roll);    // Setting initial angle to accelerometer value.
  k_pitch_estimator.setAngle(acc_pitch);

  // Start estimator in the background.
  stateEstimator.begin(updateEstimators, (int)(ESTIMATION_TIME * 1000000));
}

void loop(void) {

  unsigned long st_time = millis();

  // Read from SBUS to control quad states and read controller inputs.
  if (sbus_rx.Read()) {
    
    sbus_data = sbus_rx.ch();

    // Update armed state.
    if (map(sbus_data[ARM_CH-1], MIN_CH_VAL, MAX_CH_VAL, MIN_MOTOR_MS_VAL, MAX_MOTOR_MS_VAL) > 1500){
      armed = true;
    }else{
      armed = false;
      FrontLeft.writeMicroseconds(MIN_MOTOR_MS_VAL);
      FrontRight.writeMicroseconds(MIN_MOTOR_MS_VAL);
      BackLeft.writeMicroseconds(MIN_MOTOR_MS_VAL);
      BackRight.writeMicroseconds(MIN_MOTOR_MS_VAL);
    }

    // Update mode selection.
    if (map(sbus_data[MODE_CH-1], MIN_CH_VAL, MAX_CH_VAL, MIN_MOTOR_MS_VAL, MAX_MOTOR_MS_VAL) > 1700){
      flight_mode = 2;
      modeTimer.update(100000);
    }else if (map(sbus_data[MODE_CH-1], MIN_CH_VAL, MAX_CH_VAL, MIN_MOTOR_MS_VAL, MAX_MOTOR_MS_VAL) < 1300){
      flight_mode = 0;
      modeTimer.update(1000000);
    }else {
      flight_mode = 1;
      modeTimer.update(500000);
    }
  }

  // Switch between flight modes. Check if armed.
  if (armed == true)
  {
    // Get stick positions.
    throttle = map(sbus_data[THROTTLE_CH-1], MIN_CH_VAL, MAX_CH_VAL, MIN_MOTOR_MS_VAL, MAX_MOTOR_MS_VAL);
    des_roll = map((float)sbus_data[ROLL_CH-1], (float)MIN_CH_VAL, (float)MAX_CH_VAL, -30.0, 30.0);          // Input can be in degrees or degrees/sec depending on what flight mode we are in!!!!
    des_pitch = map((float)sbus_data[PITCH_CH-1], (float)MIN_CH_VAL, (float)MAX_CH_VAL, -30.0, 30.0);        // ^
    des_yaw = map((float)sbus_data[YAW_CH-1], (float)MIN_CH_VAL, (float)MAX_CH_VAL, -30.0, 30.0);            // ^

    // 10 point rolling average on RC inputs. Will cause command lag, but also filter out high frequency noise.
    float *avg_rc_inputs = rolling_average_rc(throttle, des_roll, des_pitch, des_yaw);
    throttle = avg_rc_inputs[0];
    des_roll = avg_rc_inputs[1];
    des_pitch = avg_rc_inputs[2];
    des_yaw = avg_rc_inputs[3];

    // Deadband controller inputs so we can actually command 0.
    if ((des_roll < .9) && (des_roll > -0.9)) des_roll = 0;
    if ((des_pitch < .9) && (des_pitch > -0.9)) des_pitch = 0;
    if ((des_yaw < .9) && (des_yaw > -0.9)) des_yaw = 0;

    // No matter what mode we are in, we will command out of the rate loop.
    std::array<float, 3> rate_loop_outputs = {0.0, 0.0, 0.0};
    
    if (flight_mode == 2) // STABILIZED MODE!
    {
      // Propogating sticks through attitude controller.
      std::array<float, 3> attitude_states = {c_roll, c_pitch, veh_yaw};
      std::array<float, 3> desired_attitude = {des_roll, des_pitch, des_yaw};
      std::array<float, 3> attitude_loop_outputs = attitude_controller.loopController(attitude_states, desired_attitude, LOOP_TIME);
  
      // Making sure our attitude controller does not blow up our rate controller with crazy desired velocities.
      for(unsigned int i = 0; i < attitude_loop_outputs.size(); i++)
      {
        if (attitude_loop_outputs[i] > MAX_RATE_COMMAND) attitude_loop_outputs[i] = MAX_RATE_COMMAND;
        if (attitude_loop_outputs[i] < MIN_RATE_COMMAND) attitude_loop_outputs[i] = MIN_RATE_COMMAND;
      }
  
      // Propogating output from attitude controller into rate controller.
      // Pay attention to signage.
      std::array<float, 3> rate_states = {gx, -gy, -gz};
      std::array<float, 3> desired_rates = {attitude_loop_outputs[0], attitude_loop_outputs[1], attitude_loop_outputs[2]};
      rate_loop_outputs = rate_controller.loopController(rate_states, desired_rates, LOOP_TIME);
      
    }else if (flight_mode == 1)   // ACRO MODE!!!
    {
      // Propogating sticks directly to rate controller. Skipping attitude controller.
      // Pay attention to signage.
      std::array<float, 3> rate_states = {gx, -gy, -gz};
      std::array<float, 3> desired_rates = {des_roll, des_pitch, des_yaw};
      rate_loop_outputs = rate_controller.loopController(rate_states, desired_rates, LOOP_TIME);
      
    }else if (flight_mode == 0)  // MODEL FREE MODE!!!
    {
      std::array<float, 6> vehicle_state = {gx, -gy, -gz, c_roll, c_pitch, veh_yaw};
      std::array<float, 3> desired_state = {des_roll, des_pitch, des_yaw};
      rate_loop_outputs = mf_controller.loopController(vehicle_state, desired_state, LOOP_TIME);
    }

    // Rate loop outputs get mixed to individual motors.
    // Saturating commanded PWM values to safe limits.
    fl = throttle + rate_loop_outputs[0] + rate_loop_outputs[1] - rate_loop_outputs[2];
    if (fl > MAX_MOTOR_MS_VAL) fl = MAX_MOTOR_MS_VAL;
    if (fl < MIN_MOTOR_MS_VAL) fl = MIN_MOTOR_MS_VAL;

    fr = throttle - rate_loop_outputs[0] + rate_loop_outputs[1] + rate_loop_outputs[2];
    if (fr > MAX_MOTOR_MS_VAL) fr = MAX_MOTOR_MS_VAL;
    if (fr < MIN_MOTOR_MS_VAL) fr = MIN_MOTOR_MS_VAL;

    bl = throttle + rate_loop_outputs[0] - rate_loop_outputs[1] + rate_loop_outputs[2];
    if (bl > MAX_MOTOR_MS_VAL) bl = MAX_MOTOR_MS_VAL;
    if (bl < MIN_MOTOR_MS_VAL) bl = MIN_MOTOR_MS_VAL;

    br = throttle - rate_loop_outputs[0] - rate_loop_outputs[1] - rate_loop_outputs[2];
    if (br > MAX_MOTOR_MS_VAL) br = MAX_MOTOR_MS_VAL;
    if (br < MIN_MOTOR_MS_VAL) br = MIN_MOTOR_MS_VAL;

    // Sending safe commands to motors
    FrontLeft.writeMicroseconds((int)fl);
    FrontRight.writeMicroseconds((int)fr);
    BackLeft.writeMicroseconds((int)bl);
    BackRight.writeMicroseconds((int)br);
  }else{

    // The system is not armed!
    // Don't let motors spin!
    FrontLeft.writeMicroseconds(1000);
    FrontRight.writeMicroseconds(1000);
    BackLeft.writeMicroseconds(1000);
    BackRight.writeMicroseconds(1000);
  }

  unsigned long end_time = millis();
  unsigned long elapsed = end_time - st_time;

  if (elapsed > (int)(LOOP_TIME * 1000)){
    Serial.println("ERROR LOOP TOOK TOO LONG!!!");
    modeTimer.update(10000);
  }else{
    delay((int)(LOOP_TIME * 1000) - elapsed); // System will run @ 400Hz or 2.5ms.
  }
}
