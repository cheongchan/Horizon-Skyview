#include "arduinoAkpParser.h"

#include <Streaming.h>
#include <SoftwareSerial.h>

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// All of these strings must be lower-case as a case-insensitive comparison
// will be done after the input is made lower case
// Text message signal to KILL
#define KILL_TEXT "kill"
// Text message signal to relay all information to subscribed numbers
#define GET_INFO_TEXT "info"
// Text message to subscribe a phone number to receive periodic texts
#define SUBSCRIBE_TEXT "subscribe"

// Buffer size for reading lines in from the cell shield
#define BUFFER_SIZE 256

// Intervals for sending out text messages
#define NORMAL_MESSAGING_INTERVAL (10UL * 60 * 1000)
#define FAST_MESSAGING_INTERVAL (2UL * 60 * 1000)

// Interval for sending tag information to arduino
#define ARDUINO_SEND_INTERVAL (1 * 1000)

// Time interval for the cell shield timing out.
// If we do not receive any input from the cell shield (line ending in \r)
// then we go through the init sequence again. This will likely time out
// some when everything is going just peachy, but it should not cause any
// trouble either to go through the sequence extra when nothing is happening anyway.
#define CELL_SHIELD_INPUT_TIMEOUT (5UL * 60 * 1000)

#define MAX_SUBSCRIBERS 6

#define BIG_ARDUINO softSerial
#define CELL_SHIELD Serial
SoftwareSerial softSerial(7, 8);

// 12033470933 is the YUAA twilio
#define ENABLE_TWILIO
char twilioPhoneNumber[] = "12033470933";
char subscribedPhoneNumbers[MAX_SUBSCRIBERS][12] = {"14018649488"};
// The index of the next slot to fill with a subscriber
// This will cycle and replace older numbers if there is no space
int subscribedNumberIndex = 1;
// The number of subscribed numbers. This will max out at MAX_SUBSCRIBERS.
int subscribedNumberCount = 1;

// 3-character Mobile Country Code
char mccBuffer[4];
// 4-character Mobile Network Code
char mncBuffer[4];

// 4-character (or so?) Location Area Code
char lacBuffer[5];
// 4-character Caller IDentifier. Can be quite long?
char cidBuffer[5];

// Latitude routed from the GPS for text message relay
char latitudeBuffer[16];
// Latitude routed from the GPS for text message relay
char longitudeBuffer[16];
// Death-time from the main controller for text message relay
char deathTimeBuffer[16];

unsigned long millisAtLastMessageRelay;
unsigned long millisAtLastArduinoSend;

// Last time a line was received from the cell shield
unsigned long lastCellShieldLineTime;

// What the current status of the LV tag from the main controller is. 0 maps to true (not lively) 1 maps to false.
// This also determines the rate that texts are sent out, between the NORMAL and the FAST intervals.
bool hasBalloonBeenKilled = false;

char getHexOfNibble(char c)
{
    c = c & 0x0f;
    if (c < 10)
    {
        return '0' + c;
    }
    else
    {
        return 'a' + c - 10;
    }
}

void sendTagCellShield(const char* tag, const char* data)
{
    unsigned char checksum = crc8(tag, 0);
    checksum = crc8(data, checksum);
    //Get hex of checksum
    char hex1 = getHexOfNibble(checksum >> 4);
    char hex2 = getHexOfNibble(checksum);

    CELL_SHIELD << tag << '^' << data << ':' << hex1 << hex2;
}

void sendTagArduino(const char* tag, const char* data)
{
    unsigned char checksum = crc8(tag, 0);
    checksum = crc8(data, checksum);
    //Get hex of checksum
    char hex1 = getHexOfNibble(checksum >> 4);
    char hex2 = getHexOfNibble(checksum);

    BIG_ARDUINO << tag << '^' << data << ':' << hex1 << hex2;
    // Small delay because that arduino will be receiving with software serial
    // that only has a buffer of 64 bytes, so we delay a little
    delay(50);
}

void sendInfoTagsToCellShield()
{
    sendTagCellShield("MC", mccBuffer);
    sendTagCellShield("MN", mncBuffer);
    sendTagCellShield("LC", lacBuffer);
    sendTagCellShield("CD", cidBuffer);
    sendTagCellShield("LA", latitudeBuffer);
    sendTagCellShield("LO", longitudeBuffer);
    sendTagCellShield("DT", deathTimeBuffer);
    sendTagCellShield("LV", hasBalloonBeenKilled ? "0" : "1");
}

void sendTextualInformationToCellShield()
{
    CELL_SHIELD << "Lat: " << latitudeBuffer << '\n';
    CELL_SHIELD << "Long: " << longitudeBuffer << '\n';
    CELL_SHIELD << (hasBalloonBeenKilled ? "DEAD!" : "ALIVE!") << '\n';
    CELL_SHIELD << "T- " << deathTimeBuffer << " seconds.\n";
    CELL_SHIELD << "MC: " << mccBuffer << "MN: " << mncBuffer << "LC: " << lacBuffer << "CD: " << cidBuffer << '\n';
}

void sendInfoTagsToArduino()
{
    // We actually don't send everything each time this function is called
    // because we do not want to overrun buffers or have similar serial communication problems
    static int sendStateOn = 0;
    switch (sendStateOn)
    {
        case 0:
            sendTagArduino("MC", mccBuffer);
            sendTagArduino("MN", mncBuffer);
            sendStateOn = 1;
            break;
        case 1:
            sendTagArduino("LC", lacBuffer);
            sendTagArduino("CD", cidBuffer);
            sendStateOn = 0;
            break;
/*        case 2:
            sendTagArduino("LA", latitudeBuffer);
            sendTagArduino("LO", longitudeBuffer);
            sendStateOn = 3;
            break;
        case 3:
            sendTagArduino("DT", deathTimeBuffer);
            sendTagArduino("LV", hasBalloonBeenKilled ? "0" : "1");
            sendStateOn = 0;
            break;*/
        default:
            // Impossible, but good form anyway
            sendStateOn = 0;
            break;
    }
    
    // For nice observation
    BIG_ARDUINO.println();
}

// Checks to see if after this additional character is added whether we have a finished input line.
// Returns the line if that is so, null if not.
char* checkForCellShieldInputLineAddChar(char c)
{
    static char commandBuffer[BUFFER_SIZE];
    static int bufferIndex = 0;
    
    if (c != -1)
    {
        BIG_ARDUINO.write(c);
    }
    switch (c)
    {
        case -1:
            // Ain't nothing here. Probably won't happen because of available check, but hey!
            return NULL;
        case '\n':
            // Ignore the newline character, which always is paired with the carriage return
            break;
        case '\r':
            // Commands are delimited by a carriage return, so we are done.
            commandBuffer[bufferIndex] = '\0';
            bufferIndex = 0;
            
            // Remember the timing of this!!!
            lastCellShieldLineTime = millis();
            
            return commandBuffer;
        default:
            // A normal character: we add it to the current command if we haven't run out of space
            if (bufferIndex < BUFFER_SIZE)
            {
                commandBuffer[bufferIndex++] = c;
            }
            else
            {
                // We have bizarely run out of buffer space! Complain!
                BIG_ARDUINO.println("AT buffer is full!");
            }
            break;
    }
    // Out of bytes and no command yet
    return NULL;
}

// Checks for characters from the cell shield and parses them into individual "commands"
// Returns the parsed command if it has just been finished. Returns null otherwise.
// (note that the char* points to an internal buffer and will thus always be the same value)
char* checkForCellShieldInputLine()
{
    static char commandBuffer[BUFFER_SIZE];
    static int bufferIndex = 0;
    
    while  (CELL_SHIELD.available())
    {
        int c = CELL_SHIELD.read();
        char* result = checkForCellShieldInputLineAddChar(c);
        if (result)
        {
            return result;
        }
    }
    // Out of bytes and no command yet
    return NULL;
}

// A blocking version of checkForCellShieldInputLine
char* cellShieldReadInputLine()
{
    char* inputLine = NULL;
    while ((inputLine = checkForCellShieldInputLine()) == NULL) {}
    return inputLine;
}

// Parses a phone number out of the given command and places it in the given buffer
// The number is found after the first " and the + that follow it.
// The number, still as a string, is put into the incoming phone number variable
void parsePhoneNumber(const char* command, char* phoneNumberBuffer) {
    char* startCharacter = strchr(command, '"') + 2;
    // Take at most a full 11 characters
    strncpy(phoneNumberBuffer, startCharacter, min(11, strlen(startCharacter)));
    // Make sure final null byte is placed
    phoneNumberBuffer[11] = '\0';
}

// Waits up to waitMillis time for a specific response to be contained in a line from the cell shield
// Returns 0 for receiving it fine, -1 for cell shield fatal errors, 1 for requested reattempt/timeout
int cellShieldWaitForResponse(const char* response, unsigned int waitMillis)
{
    unsigned long startTime = millis();
    
    while (millis() - startTime < waitMillis)
    {
        const char* inputLine = checkForCellShieldInputLine();
        if (inputLine)
        {
            if (strstr(inputLine, response))
            {
                return 0;
            }
            else
            {
                int status = handleCellShieldCommand(inputLine);
                if (status) return status;
            }
        }
    }
    
    BIG_ARDUINO << "Response wait timeout\n";
    return 1;
}

// Waits for a < character as the prompt for when the cell shield is ready to send a text message
// Processes received lines normally in the mean time.
// Returns 0 on success, -1 for cell failure, 1 for no cell service/to retry sending the text message
int cellShieldWaitForTextReady() {
    unsigned long startTime = millis();
    
    while (millis() - startTime < 1000)
    {
        if (CELL_SHIELD.available() > 0) {
            char c = CELL_SHIELD.read();
            if (c == '>')
            {
                return 0;
            }
            else
            {
                // Keep track of the bytes anyhow and see if we get something interesting
                char* inputLine = checkForCellShieldInputLineAddChar(c);
                int result = handleCellShieldCommand(inputLine);
                // If we get an unusual result, return it!
                if (result)
                {
                    return result;
                }
            }
        }
    }
    
    // Try again?
    return 1;
}

int startTextMessage(const char* phoneNumber) {
    BIG_ARDUINO << "Starting text to " << phoneNumber << "\n";
    CELL_SHIELD << "AT+CMGS=\"" << phoneNumber << "\"\r\n";
    return cellShieldWaitForTextReady();
}

int endTextMessage() {
    // A special stop byte
    CELL_SHIELD.write(26);
    
    // We wait until we have received the returned CMGS indicating the entire operation is completed
    // This may take a while!
    int status = cellShieldWaitForResponse("CMGS", 10000);
    if (status) return status;
    
    status = cellShieldWaitForResponse("OK", 1000);
    if (status) return status;
    
    BIG_ARDUINO.println("Text successfull!");
    
    return 0;
}

int sendTextMessageTry(const char* phoneNumber, bool asTags)
{
    int result = startTextMessage(phoneNumber);
    if (result) return result;
    
    BIG_ARDUINO << "Proceeding with text\n";
    
    if (asTags)
    {
        sendInfoTagsToCellShield();
    }
    else
    {
        sendTextualInformationToCellShield();
    }
    return endTextMessage();
}

int sendTextMessage(const char* phoneNumber, bool asTags)
{
    int result = sendTextMessageTry(phoneNumber, asTags);
    while (result == 1)
    {
        BIG_ARDUINO << "No service or PUK; retrying...\n";
        delay(1000);
        result = sendTextMessageTry(phoneNumber, asTags);
    }
    return result;
}

int sendTextMessages()
{
    #ifdef ENABLE_TWILIO
    sendTextMessage(twilioPhoneNumber, true);
    #endif
    
    for (int i = 0; i < subscribedNumberCount; i++)
    {
        if (sendTextMessage(subscribedPhoneNumbers[i], false)) return -1;
    }
    BIG_ARDUINO << "All subscribers messaged\n";
    return 0;
}

// Handles a command
// Returns 0 if nothing bad has happened
// and -1 if the cell shield has errored and needs to be reset
// and 1 if the cell shield has no service and ought to try sending a text again.
int handleCellShieldCommand(const char* command) {
    if (strstr(command,"ERROR"))
    {
        if (strstr(command, "CME ERROR: 30"))
        {
            // Error 30 means we have no cellular service
            return 1;
        }
        else if (strstr(command, "CMS ERROR: 313"))
        {
            // Weird PUK thing.. we should try again
            return 1;
        }
        else
        {
            BIG_ARDUINO.println("Cell shield error");
            return -1;
        }
    }
    
    // Did we just get a text message?
    if (strstr(command,"+CMT"))
    {
        BIG_ARDUINO.println("Parsing phone number");
        char incomingPhoneNumber[12];
        parsePhoneNumber(command, incomingPhoneNumber);
        
        // We want to read in another line (command)
        // This line is the contents of the text message itself
        char* message = cellShieldReadInputLine();
        
        // Make the command all lower case for case-insensitive comparisons to occur
        // (our defined TEXTs are all lower case)
        char* messageCharOn = message;
        while (*messageCharOn)
        {
            *messageCharOn = tolower((unsigned char) *messageCharOn);
            messageCharOn++;
        }
        
        if (strstr(message, KILL_TEXT))
        {
            // Send the kill tag to the arduino... Three times in a row! Wheeeee! (to make sure he dies!)
            sendTagArduino("KL", "");
            sendTagArduino("KL", "");
            sendTagArduino("KL", "");
            // We will inform people about the killing once it is successful!
            // In the mean time, we do want to enter alert mode!
            hasBalloonBeenKilled = true;
            BIG_ARDUINO << "Alert mode\n";
        }
        if (strstr(message, GET_INFO_TEXT))
        {
            BIG_ARDUINO << "Sending info to " << incomingPhoneNumber << '\n';
            if (sendTextMessage(incomingPhoneNumber, false)) return -1;
        }
        if (strstr(message, SUBSCRIBE_TEXT))
        {
            BIG_ARDUINO << "Subscribing to texts: " << incomingPhoneNumber << '\n';
            addSubscriber(incomingPhoneNumber);
        }
        
        // Delete the text message we just received
        BIG_ARDUINO << "Deleting received text\n";
        if (sendToCellShieldAndConfirm("AT+CMGD=1,4")) return -1;
        BIG_ARDUINO << "Done\n";
    }
    
    if (strstr(command, "+CREG"))
    {
        BIG_ARDUINO.println("Received CREG");
        storeLacAndCid(command);
    }
    
    if (strstr(command, "+COPS"))
    {
        BIG_ARDUINO.println("Received COPS");
        storeMccAndMnc(command);
    }
    
    return 0;
}

// Sends the cell shield the given command plus \r\n
// Reads in commands and allows them to be handled,
// waiting for an OK response before returning
// Times out and resends the command if no OK is received in 1 second
// Returns -1 for cell shield fatal error, 0 otherwise
int sendToCellShieldAndConfirm(const char* command)
{
    while (1)
    {
        unsigned long startTime = millis();
        CELL_SHIELD.println(command);
        BIG_ARDUINO << "Sending to cell shield: " << command << "\n";
        
        while (millis() - startTime < 1000)
        {
            const char* inputLine = checkForCellShieldInputLine();
            if (inputLine)
            {
                if (strstr(inputLine, "OK"))
                {
                    // Delay a little to give it preparation before another command
                    delay(500);
                    return 0;
                }
                else
                {
                    if (handleCellShieldCommand(inputLine)) return -1;
                }
            }
        }
        BIG_ARDUINO << "Cell shield timed out. Resending.\n";
    }
}

int requestAndStoreMccAndMnc() {
    while (1)
    {
        if (sendToCellShieldAndConfirm("AT+COPS=0")) return -1;
        CELL_SHIELD.println("AT+COPS?");
        
        unsigned long startTime = millis();
        
        while (millis() - startTime < 1000)
        {
            const char* inputLine = checkForCellShieldInputLine();
            if (inputLine)
            {
                if (handleCellShieldCommand(inputLine)) return -1;
                if (strstr(inputLine, "COPS"))
                {
                    // Delay in prep of the next thing
                    delay(500);
                    return 0;
                }
            }
        }
    }
}

void storeMccAndMnc(const char* inputLine)
{
    char *firstComma = strchr(inputLine,',');
    if (firstComma)
    {
        // The codes we are looking for are located after the second comma
        char *mccLocation = strchr(firstComma + 1, ',') + 1;
        
        // Extract MCC
        memcpy(mccBuffer, mccLocation, 3);
        mccBuffer[3] = '\0';
        
        // Extract MNC
        memcpy(mncBuffer, mccLocation + 3, 3);
        mncBuffer[3] = '\0';
        
        BIG_ARDUINO << "MCC: " << mccBuffer << " MNC: " << mncBuffer << '\n';
    }
}

void storeLacAndCid(const char* command)
{
    // We want a valid CREG, which looks like: "+CREG: 1,0x1395,0xD7D4"
    // We check by verifying the length
    if (strlen(command) != 22)
    {
        // Bad!
        BIG_ARDUINO << "Bad CREG!\n";
        return;
    }
    
    // We want to skip past the ,0x
    char *lacLocation = strchr(command, ',') + 3;
    
    // Extract LAC
    memcpy(lacBuffer, lacLocation, 4);
    lacBuffer[4] = '\0';
    
    // Extract CID (there is a comma between the lac and cid)
    // Also, we again skip the 0x
    memcpy(cidBuffer, lacLocation + 7, 4);
    cidBuffer[4] = '\0';
    
    BIG_ARDUINO << "LAC: " << lacBuffer << " CID: " << cidBuffer << '\n';
}

void addSubscriber(const char* phoneNumber)
{
    strncpy(subscribedPhoneNumbers[subscribedNumberIndex], phoneNumber, 11);
    subscribedPhoneNumbers[subscribedNumberIndex][11] = '\0';
    
    subscribedNumberIndex = (subscribedNumberIndex + 1) % MAX_SUBSCRIBERS;
    subscribedNumberCount = (subscribedNumberCount >= MAX_SUBSCRIBERS) ? MAX_SUBSCRIBERS : subscribedNumberCount + 1;
}

int sendInfoIfItsTimeTo()
{
    long unsigned delayTime = hasBalloonBeenKilled ? FAST_MESSAGING_INTERVAL : NORMAL_MESSAGING_INTERVAL;
    
    if (millis() - millisAtLastArduinoSend >= ARDUINO_SEND_INTERVAL)
    {
        millisAtLastArduinoSend = millis();
        sendInfoTagsToArduino();
    }
    
    //BIG_ARDUINO << "DelayTime: " << delayTime << " lastRelay: " << millisAtLastMessageRelay << '\n';
    
    if (millis() - millisAtLastMessageRelay >= delayTime)
    {
        millisAtLastMessageRelay = millis();
        if (sendTextMessages()) return -1;
    }
    
    return 0;
}

int setupSim()
{
    // A nice little delay for when this is called repeatedly because of errors
    delay(500);
    
    // Now make sure we have parsed all incoming data and gotten it through and out of our system!
    char* inputLine;
    while (inputLine = checkForCellShieldInputLine())
    {
        handleCellShieldCommand(inputLine);
    }
    
    // Setting text mode
    BIG_ARDUINO.println("Resetting SIM: setting...");
    
    BIG_ARDUINO.println("..to AT&T freq.");
    if (sendToCellShieldAndConfirm("AT+SBAND=7")) return -1;
    BIG_ARDUINO.println("Done");
    
    BIG_ARDUINO.println("..to text mode");
    if (sendToCellShieldAndConfirm("AT+CMGF=1")) return -1;
    BIG_ARDUINO.println("Done");
    
    BIG_ARDUINO.println("..to ASCII");
    if (sendToCellShieldAndConfirm("AT+CSMP=,,,0")) return -1;
    BIG_ARDUINO.println("Done");
    
    // Delete all pre-existing text messages
    BIG_ARDUINO.println("Deleting old messages");
    if (sendToCellShieldAndConfirm("AT+CMGD=1,4")) return -1;
    BIG_ARDUINO.println("Done");
    
    // Request general carrier and country specific codes now.
    // (They really shouldn't change)
    if (requestAndStoreMccAndMnc()) return -1;
    
    // Settings for receiving text messages
    BIG_ARDUINO.println("..text settings");
    if (sendToCellShieldAndConfirm("AT+CNMI=3,3,0,0")) return -1;
    BIG_ARDUINO.println("Done");
    
    // Request a report with the LAC and CID
    BIG_ARDUINO.println("Requesting CREGs");
    if (sendToCellShieldAndConfirm("AT+CREG=2")) return -1;
    BIG_ARDUINO.println("Done");
    
    return 0;
}

void setup() {
    BIG_ARDUINO.begin(28800);
    CELL_SHIELD.begin(9600);
    
    BIG_ARDUINO.println("Starting init");
    
    millisAtLastMessageRelay = millis();
    millisAtLastArduinoSend = millis();
    
    // Keep up trying to setup the cell shield as much as necessary!
    while (setupSim());
    
    BIG_ARDUINO.println("Init complete");
}

void checkForControllerInput()
{
    static TagParseData tpData;
    
    // Handle all the bytes that are currently available
    while (BIG_ARDUINO.available())
    {
        // Read a character from the controller and give it to the parser
        // If we have a parsed tag finished with this character, then respond accordingly
        int c = BIG_ARDUINO.read();
        BIG_ARDUINO.write((char)c);
        if (parseTag(c, &tpData))
        {
            BIG_ARDUINO << "Got: " << tpData.tag << " with: " << tpData.data << '\n';
            if (strcmp(tpData.tag, "LV") == 0)
            {
                
                // We only care for the first byte on the liveliness tag, and we only care whether it is not 0
                bool imToldImDead = (tpData.data[0] == '0');
                if (!hasBalloonBeenKilled && imToldImDead)
                {
                    hasBalloonBeenKilled = true;
                    // We send texts at a faster rate!
                    BIG_ARDUINO << "Alert mode!\n";
                    
                    // Give an immediate SMS update
                    sendTextMessages();
                }
                else if (hasBalloonBeenKilled && !imToldImDead)
                {
                    // The mains were likely intentionally reset.
                    // We should be subservient to them.
                    hasBalloonBeenKilled = false;
                    BIG_ARDUINO << "Alert mode off!\n";
                    
                    // Give an immediate SMS update
                    sendTextMessages();
                }
            }
            else if (strcmp(tpData.tag, "LA") == 0)
            {
                strncpy(latitudeBuffer, tpData.data, sizeof(latitudeBuffer) - 1);
                latitudeBuffer[sizeof(latitudeBuffer) - 1] = '\0';
            }
            else if (strcmp(tpData.tag, "LO") == 0)
            {
                strncpy(longitudeBuffer, tpData.data, sizeof(longitudeBuffer) - 1);
                longitudeBuffer[sizeof(longitudeBuffer) - 1] = '\0';
            }
            else if (strcmp(tpData.tag, "DT") == 0)
            {
                strncpy(deathTimeBuffer, tpData.data, sizeof(deathTimeBuffer) - 1);
                deathTimeBuffer[sizeof(deathTimeBuffer) - 1] = '\0';
            }
        }
    }
}

void loop()
{
    
    if (millis() - lastCellShieldLineTime >= CELL_SHIELD_INPUT_TIMEOUT)
    {
        // Do setup again!
        while (setupSim());
    }
    
    char* inputLine = checkForCellShieldInputLine();
    if (inputLine)
    {
        if (handleCellShieldCommand(inputLine))
        {
            
            // Cell-shield has had a bad error. Try to reset it and continue.
            // If it fails... just keep on trying!
            while (setupSim());
        }
    }
    
    checkForControllerInput();
    
    if (sendInfoIfItsTimeTo())
    {
        while (setupSim());
    }
}
