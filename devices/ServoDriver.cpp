#include "ServoDriver.h"

ServoDriver::ServoDriver()
{
    
}

void ServoDriver::setAngle(int16_t degrees)
{
    
}

void ServoDriver::setPosition(int16_t servo, int16_t position)
{
//Derived from sample code found online
// Set a servo position.  Returns 0 on success, -1 on error
// Uses the Pololu protocol, so the jumper has to be off of the controller
// Makes use of "Absolute Position" command
// VALID POSITION VALUES: 0-5000

//from arduino to Beaglebone changed write to fputs
  byte data1 = 0;
  byte data2 = 0;
 
  // Check to make sure the servo is right
  if (servo <0 || servo >8)
   return -1;
 
  // Check to make sure position is within bounds
  if (position < 0 || position > 5000)
    return -1;
 
  // Controller actually takes values between 500 and 5500,
  // so just add 500
  position+=500;
  // Calculate data bytes from position
  data2 = position & B01111111;
  data1 = position >> 7;
 
 // Start Byte - always the same for pololu controller
  fputs (0x80, );
  
  // Device ID - Device ID number 0x01 for 8-Servo Controller
  fputs (0x01, );
  
  // Command: 0x04 is set position mode
  fputs (0x04, );
  
  // Servo number
  fputs (servo, );
  
  // First data byte
  fputs (data1, );
  
  // Second data byte
  fputs (data2, );
 
  // Everything seems ok, return 0
  return (0);
}

void ServoDriver::setSpeed(int16_t speed, int16_t servo)
{
//Set Speed Function for the Servo. Returns 0 on success, -1 on error
//Set speed from 0 to 127 with 0 being the default speed
//Arduino code is working so converted write() to fputs () but need to continue to do more
 
  // Check to make sure the servo is right
  if (servo <0 || servo >8)
   return -1;
 
  // Check to make sure speed is within bounds between 1 and 127
  if (speed < 0 || speed > 127)
    return -1;
 
  // Start Byte - always the same for pololu controller
  fputs (0x80, );
  
  // Device ID - Device ID number 0x01 for 8-Servo Controller
  fputs (0x01, );
  
  // Command: 0x01 is set speed mode change
  fputs (0x01, );
  
  // Servo number
  fputs (servo, );
  
  // First data byte: the value of the speed
  fputs (speed, );
  
 // Everything seems ok, return 0
 return (0); 
}

int16_t ServoDriver::getAngle() const
{
    
}
