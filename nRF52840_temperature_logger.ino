#include <SPI.h>
#include <Wire.h>
#include <Arduino.h>
#include <Adafruit_SPIFlash.h>
#include <flash_devices.h>
#include <Adafruit_FlashTransport.h>
#include <Adafruit_FlashCache.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <nrfx_temp.h>
#include <nrf.h>
#include <Adafruit_TMP119.h>

#include "RTClib.h"
#include "SdFat_Adafruit_Fork.h"
#include "Adafruit_TinyUSB.h"
#include "ff.h"
#include "diskio.h"
#include "flash_config.h" // for flashTransport definition

#if defined ARDUINO_NRF52840_CIRCUITPLAY
#define  PIN_VBAT          A8   // this is just a mock read, we'll use the light sensor, so we can run the test
#endif
#define VBAT_MV_PER_LSB   (0.73242188F)   // 3.0V ADC range and 12-bit ADC resolution = 3000mV/4096
#define VBAT_DIVIDER      (0.5F)          // 150K + 150K voltage divider on VBAT
#define VBAT_DIVIDER_COMP (2.0F)          // Compensation factor for the VBAT divider
#define REAL_VBAT_MV_PER_LSB (VBAT_DIVIDER_COMP * VBAT_MV_PER_LSB)

#define RTC_INTERRUPT_PIN 10                //
#define MAX_LEVEL   3

#define DISK_LABEL "DATA"                   // up to 11 characters
#define DATA_FILENAME "/data"               // the file where data will be stored
#define SAMPLING_CFG_FILENAME    "/config"  // the file where config will be stored
#define SAMPLE_FLAG_FILENAME "sample_on"
#define MIN_SPI_FLASH_FREE_BYTES 10    // mininum space left in SPI flash before stopping sampling
#define BUTTON_PIN 7

uint32_t samplingFlag = 0; 

Adafruit_TMP119 tmp11x;

Adafruit_SPIFlash flash(&flashTransport);
FatVolume fatfs; // file system object from SdFat


typedef struct
{
  unsigned long time;    // using custom epoch January 1, 2026 (1767225600)
  unsigned int  vbat;
  int  rtcTemp;
  int  temp;
} storeSample;

typedef struct
{
  int samplingIntervalSecs;    
  bool ledOnSampling;
} samplingConfig;


uint32_t vbat_pin = PIN_VBAT;             // A7 for feather nRF52832, A6 for nRF52840

nrfx_temp_config_t config = NRFX_TEMP_DEFAULT_CONFIG;

TwoWire *wi = &Wire;


// USB Mass Storage object
Adafruit_USBD_MSC usb_msc;

// Check if flash is formatted
bool fs_formatted = false;

// Set to true when PC write to flash
bool fs_changed = true;;


RTC_DS3231 rtc;

unsigned long sampleTime = -999;
float sampleRtcTemp = -999;
float sampleTemp = -999;
float sampleVbat = -999.9;


int SamplingIntervalMenu();


float readVBAT(void) {
  float raw;

  // Set the analog reference to 3.0V (default = 3.6V)
  analogReference(AR_INTERNAL_3_0);

  // Set the resolution to 12-bit (0..4095)
  analogReadResolution(12); // Can be 8, 10, 12 or 14

  // Let the ADC settle
  delay(1);

  // Get the raw 12-bit, 0..3000mV ADC value
  raw = analogRead(vbat_pin);

  // Set the ADC back to the default settings
  analogReference(AR_DEFAULT);
  analogReadResolution(10);

  // Convert the raw value to compensated mv, taking the resistor-
  // divider into account (providing the actual LIPO voltage)
  // ADC range is 0..3000mV and resolution is 12-bit (0..4095)
  return raw * REAL_VBAT_MV_PER_LSB;
}


void format_fat12(void) {
  // Working buffer for f_mkfs.
  #ifdef __AVR__
    uint8_t workbuf[512];
  #else
    uint8_t workbuf[4096];
  #endif

  // Elm Cham's fatfs objects
  FATFS elmchamFatfs;

  // Make filesystem.
  FRESULT r = f_mkfs("", FM_FAT, 0, workbuf, sizeof(workbuf));
  if (r != FR_OK) {
    Serial.print(F("Error, f_mkfs failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // mount to set disk label
  r = f_mount(&elmchamFatfs, "0:", 1);
  if (r != FR_OK) {
    Serial.print(F("  Error, f_mount failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // Setting label
  Serial.println(F("  Setting disk label to: " DISK_LABEL));
  r = f_setlabel(DISK_LABEL);
  if (r != FR_OK) {
    Serial.print(F("Error, f_setlabel failed with error code: "));
    Serial.println(r, DEC);
    while (1)
      yield();
  }

  // unmount
  f_unmount("0:");

  // sync to make sure all data is written to flash
  flash.syncBlocks();

  Serial.println(F("  Formatted flash!"));
}


void printRTCtime() {
  DateTime now = rtc.now();
  Serial.print("  DS3231 Time = ");
  Serial.print(now.year(), DEC);
  Serial.print('/');
  Serial.print(now.month(), DEC);
  Serial.print('/');
  Serial.print(now.day(), DEC);
  Serial.print(" ");
  Serial.print(now.hour(), DEC);
  Serial.print(':');
  Serial.print(now.minute(), DEC);
  Serial.print(':');
  Serial.print(now.second(), DEC);
  Serial.println();
}

void check_fat12(void) {
  // Check new filesystem
  if (!fatfs.begin(&flash)) {
    Serial.println(F("  Error, failed to mount newly formatted filesystem!"));
    while (1)
      delay(1);
  }
}

void RTCStatus() {

  Serial.print("  DS3231 RTC temperature: ");
  Serial.print(rtc.getTemperature());
  Serial.println(" C");
}







/**************************************************************************/
/*!
    @brief  Print out whole directory tree of an folder
            until the level reach MAX_LEVEL

    @note   Recursive call
*/
/**************************************************************************/
void printTreeDir(const char* cwd, uint8_t level)
{
  // Open the input folder
  Adafruit_LittleFS_Namespace::File dir(cwd, Adafruit_LittleFS_Namespace::FILE_O_READ, InternalFS);

  // Print root
  if (level == 0) Serial.println("  root");
 
  // File within folder
  Adafruit_LittleFS_Namespace::File item(InternalFS);

  // Loop through the directory 
  while( (item = dir.openNextFile(Adafruit_LittleFS_Namespace::FILE_O_READ)) )
  {
    // Indentation according to dir level
    for(int i=0; i<level; i++) Serial.print("  |  ");

    Serial.print("  |_ ");
    Serial.print( item.name() );

    if ( item.isDirectory() )
    {
      Serial.println("  /");

      // ATTENTION recursive call to print sub folder with level+1 !!!!!!!!
      // High number of MAX_LEVEL can cause memory overflow
      if ( level < MAX_LEVEL )
      {
        char dpath[strlen(cwd) + strlen(item.name()) + 2 ];
        strcpy(dpath, cwd);
        strcat(dpath, "/");
        strcat(dpath, item.name());
        
        printTreeDir( dpath, level+1 );
      }
    }else
    {
      // Print file size starting from position 30
      int pos = level*3 + 3 + strlen(item.name());

      // Print padding
      for (int i=pos; i<30; i++) Serial.print(' ');

      // Print at least one extra space in case current position > 50
      Serial.print(' ');
      
      Serial.print( item.size() );
      Serial.println( " Bytes");
    }

    item.close();
  }

  dir.close();
}

void scanI2C() {
  Serial.println("  Scanning address from 0 to 127");
  for (int addr = 1; addr < 128; addr++)
  {
    wi->beginTransmission(addr);
    if ( 0 == wi->endTransmission() )
    {
      Serial.print("  Found: 0x");
      Serial.print(addr, HEX);
      Serial.println();
    }
  }
}


void displayVBAT() {
  
  // Get a raw ADC reading
  float vbat_mv = readVBAT();

  // Display the results
  Serial.print("  Vbat = ");
  Serial.print(vbat_mv);
  Serial.println(" mV ");
}

void displayNrfxTemp() {
  
  // Get the reading
  nrfx_temp_measure(); // In blocking mode: this function waits until the measurement is finished.
  int32_t raw_temp = nrfx_temp_result_get();
  float temp_c = raw_temp / 4.0;
  
  // Display the results
  Serial.print("  NRFX temperature: ");
  Serial.print(temp_c);
  Serial.println(" C");
}

uint64_t SPIFlash_free() {
  
  // Calculate free space
  uint32_t freeClusters = fatfs.freeClusterCount();
  uint32_t sectorsPerCluster = fatfs.sectorsPerCluster();
  uint32_t bytesPerSector = 512; // Standard for FAT filesystems

  // Total free capacity left in bytes
  uint64_t freeBytes = (uint64_t)freeClusters * sectorsPerCluster * bytesPerSector;

  return freeBytes;
}

void formatSPIFlash() {
  
  Serial.println("  Erasing SPIFlash chip in 10 seconds...");
  Serial.println("  Note you will see stat and other debug output printed repeatedly.");
  Serial.println("  Let it run for ~30 seconds until the flash erase is finished.");
  Serial.println("  An error or success message will be printed when complete.");

  if (!flash.eraseChip()) {
    Serial.println("  Failed to erase chip!");
    return;
  }

  flash.waitUntilReady();
  Serial.println("  Successfully erased chip!");

  // Call fatfs begin and passed flash object to initialize file system
  Serial.println(F("  Creating and formatting FAT filesystem (this takes ~60 seconds)..."));

  format_fat12();
  check_fat12();

  Serial.println(F("  Flash chip successfully formatted with new empty filesystem!"));
 }



void sample() { 

  DateTime now = rtc.now();

  if (tmp11x.begin()) {
    while (!tmp11x.dataReady()) {
      delay(10);
    }
    sensors_event_t temp; 
    tmp11x.getEvent(&temp); // fill the empty event object with the current measurements
    sampleTemp = temp.temperature;
  } else {
    sampleTemp = -999;
  }

  sampleRtcTemp = rtc.getTemperature();
  sampleTime = now.unixtime();
  sampleVbat = readVBAT();
  
}

void printSample() { 
  
  Serial.print(F("  sample UNIXTIME: " ));
  Serial.println(sampleTime);
  Serial.print(F("  sample RTC Temperature [C]: "));
  Serial.println(sampleRtcTemp);
  Serial.print(F("  sample TMP119 Temperature [C]: "));
  Serial.println(sampleTemp);
  Serial.print(F("  sample Vbat [mv]: " ));
  Serial.println(sampleVbat);
  
}


void printDataFile() { 

  File32 dataFile = fatfs.open(DATA_FILENAME, FILE_READ);
  if (!dataFile) {
    Serial.println(F("  Error, failed to open " DATA_FILENAME " for reading!"));
  } else {
    
    // binary format
    storeSample smp = {};
    while (dataFile.available()) {
      const size_t bytesRead = dataFile.read((byte *)&smp, sizeof(smp));
      Serial.print((smp.time * 2)+1767225600);
      Serial.print(",");
      Serial.print(smp.vbat/100.0);
      Serial.print(",");
      Serial.print(smp.rtcTemp/100.0);
      Serial.print(",");
      Serial.println(smp.temp/100.0);

    }  
  }
}



void setSamplingConfig() {

   int  samplingIntervalSecs = SamplingIntervalMenu();
   bool ledOnSampling = ledOnSampleMenu();

   samplingConfig config = {
    samplingIntervalSecs,
    ledOnSampling
    };

   Serial.print(F("  Setting interval to " ));
   Serial.print(config.samplingIntervalSecs);
   Serial.println(F(" seconds" ));
   if (config.ledOnSampling) {
      Serial.println(F("  LED on sampling: ON" ));
   } else {
      Serial.println(F("  LED on sampling: OFF" ));
   }

   if (fatfs.exists(SAMPLING_CFG_FILENAME)) {
     if (!fatfs.remove(SAMPLING_CFG_FILENAME)) {
        Serial.println("   Failed to delete old config file");
     }
   }

   File32 writeFile = fatfs.open(SAMPLING_CFG_FILENAME, FILE_WRITE);
   if (!writeFile) {
      Serial.println(F("Error, failed to open " SAMPLING_CFG_FILENAME " for writing!"));
      while (1) {
        yield();
      }
  }
  Serial.println(F("  Writing config to file " SAMPLING_CFG_FILENAME ));

  // writeFile.write((uint8_t *)&config, sizeof(config));

  writeFile.println(samplingIntervalSecs, DEC);
  writeFile.println(ledOnSampling);
  
  // Close the file when finished writing.
  writeFile.close();
   
}


samplingConfig readConfig() {
  
  samplingConfig config = {-1,false};
  // reads sampling parameters from SPIFlash config file
  File32 readFile = fatfs.open(SAMPLING_CFG_FILENAME, FILE_READ);
  if (!readFile) {
      Serial.println("  Can't find config file !!!!");
      return config;
  }
  
  // const size_t bytesRead = readFile.read((uint8_t *)&config, sizeof(config));
  // Serial.println(bytesRead);

  String line;
  line = readFile.readStringUntil('\n');
  int samplingIntervalSecs = line.toInt();
  line = readFile.readStringUntil('\n');
  bool ledOnSampling = line.toInt() > 0;

  readFile.close();

  config = {
    samplingIntervalSecs,
    ledOnSampling
    };

  return config;
}


void showSamplingConfig() {
   samplingConfig config = readConfig();
   Serial.print("  Sampling interval [seconds]: ");
   Serial.println(config.samplingIntervalSecs);
   if (config.ledOnSampling){
      Serial.println("    LED on sampling: ON");
   } else {
      Serial.println("    LED on sampling: OFF");
   }
   
}


void led_signal1() {
  
  digitalWrite(LED_RED, LOW);
  for(int i=0; i<5; i++) {
        digitalToggle(LED_RED); // Toggle LED 
        delay(1000);   
      }
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, HIGH);
  for(int i=0; i<21; i++) {
        digitalToggle(LED_RED); // Toggle LED 
        digitalToggle(LED_BLUE); // Toggle LED 
        delay(100);   
      }
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, LOW);

}

void led_signal2() {
  
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, HIGH);
  for(int i=0; i<10; i++) {
        digitalToggle(LED_RED); // Toggle LED 
        digitalToggle(LED_BLUE); // Toggle LED 
        delay(200);   
      }
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, LOW);

}

void led_signal3() {
  
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, HIGH);
  for(int i=0; i<7; i++) {
        digitalToggle(LED_RED); // Toggle LED 
        digitalToggle(LED_BLUE); // Toggle LED 
        delay(200);   
      }
//  digitalWrite(LED_RED, LOW);
//  for(int i=0; i<7; i++) {
//        digitalToggle(LED_BLUE); // Toggle LED 
//        delay(200);   
//      }
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, LOW);
  
}


void led_signal4() {
  
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, LOW);
  for (int i=0; i<2; i++) {
        digitalWrite(LED_BLUE, LOW);
        for(int j=0; j<7; j++) {
          digitalToggle(LED_BLUE); // Toggle LED 
          delay(100);   
        }
        digitalWrite(LED_BLUE, LOW);
        delay(500);
  }
}


const char * menu_banner = "\n \
  |****************************| \n \
  |**|    Configuration     |**| \n \
  |**|        Menu          |**| \n \
  |****************************| \n \
  \n \
  Select one of the following options: \n \
  0 Status \n \
  1 InternalFS List Files \n \
  2 SPIFlash List Files \n \
  3 Format InsternalFS \n \
  4 Erase & format SPIFlash \n \
  5 Scan I2C \n \
  6 Set sampling config \n \
  7 Show sampling config \n \
  8 One-shot sample \n \
  9 Print data file \n \
  R Run sampling \n";
  


void option0() {
  printStatus();
  }

void option1() {
  printTreeDir("/", 0);
  }

void option3() {
  Serial.print("  Formating InternalFS... ");
  delay(1); 
  
  // Format 
  InternalFS.format();
  Serial.println(" done! ");
  }

void option2() {
  printSPIflashTreeDir();
  }

void option4() {
  formatSPIFlash();
 }

void option5() {
  scanI2C();
 }

void option6() {
  setSamplingConfig();  
 }

void option7() {
  showSamplingConfig();  
 }

void option8() {
  oneShotSample();  
  }

void option9() {
  printDataFile();  
  }


bool Menu() {
    Serial.println(menu_banner);
    for (;;) {
        switch (Serial.read()) {
            case '0': option0(); led_signal3(); Serial.println(menu_banner); break;
            case '1': option1(); Serial.println(menu_banner); break;
            case '2': option2(); Serial.println(menu_banner); break;
            case '3': option3(); Serial.println(menu_banner); break;
            case '4': option4(); Serial.println(menu_banner); break;
            case '5': option5(); Serial.println(menu_banner); break;
            case '6': option6(); Serial.println(menu_banner); break;
            case '7': option7(); Serial.println(menu_banner); break;
            case '8': option8(); Serial.println(menu_banner); break;
            case '9': option9(); Serial.println(menu_banner); break;
            case 'R': return startSampling(); 
            default: continue;  // includes the case 'no input'
        }
    }
}

const char * sampling_banner = "\n \
  Choose sampling interval: \n \
  1 10 seconds \n \
  2 30 seconds \n \
  3 60 seconds \n \
  4 120 seconds \n \
  5 300 seconds \n \
  6 600 seconds \n ";

int SamplingIntervalMenu() {
    Serial.println(sampling_banner);
    for (;;) {
        switch (Serial.read()) {
            case '1': return 10; break;
            case '2': return 30; break;
            case '3': return 60; break;
            case '4': return 120; break;
            case '5': return 300; break;
            case '6': return 600; break;
            default: continue;  // includes the case 'no input'
        }
    }
}


bool ledOnSampleMenu() {
  Serial.print("  LED on sampling [Y/N] ?");
  for (;;) {
      switch (Serial.read()) {
          case 'y': Serial.println(" y"); return true; break;
          case 'Y': Serial.println(" Y"); return true; break;
          case 'n': Serial.println(" n"); return false; break;
          case 'N': Serial.println(" N"); return false; break;
          default: continue;  // includes the case 'no input'
      }
  }
}

void oneShotSample() {
  sample(); 
  printSample();
  uint32_t freeBytes = SPIFlash_free();
  if (freeBytes > MIN_SPI_FLASH_FREE_BYTES) {
    writeSample();
  }
}

bool writeSample() { 
  
  // writes sampling data to data file
  File32 writeFile = fatfs.open(DATA_FILENAME, FILE_WRITE);
  if (!writeFile) {
    return false;
  }

  // binary format
  storeSample smp = {
    (unsigned long)(sampleTime - 1767225600) / 2,
    (unsigned int)(sampleVbat*100),
    (int)(sampleRtcTemp*100),
    (int)(sampleTemp*100)
  };
  writeFile.write((const uint8_t *)&smp, sizeof(smp));

  
//
//  // text format
//  writeFile.print(sampleTime,DEC);
//  writeFile.print(",");
//  writeFile.print(sampleVbat,DEC);
//  writeFile.print(",");
//  writeFile.println(sampleRtcTemp,DEC);
//  writeFile.print(",");
//  writeFile.println(sampleTemp,DEC);

  
  writeFile.close();
  return true;
}

bool startSampling() {
  
  samplingConfig config = readConfig();
  if (config.samplingIntervalSecs > 0) {
    samplingFlag = 1;
    Adafruit_LittleFS_Namespace::File file(InternalFS);
    if ( file.open(SAMPLE_FLAG_FILENAME, Adafruit_LittleFS_Namespace::FILE_O_WRITE)) {
      file.write((uint8_t*)&samplingFlag, sizeof(samplingFlag));
      file.close();
      led_signal1(); 
      return true;
    } 
  }
  return false;
}


void stopSampling() {
  
//  samplingFlag = 0;
//  Adafruit_LittleFS_Namespace::File file(InternalFS);
//  if ( file.open(SAMPLE_FLAG_FILENAME, Adafruit_LittleFS_Namespace::FILE_O_WRITE)){
//    file.write((uint8_t*)&samplingFlag, sizeof(samplingFlag));
//  }
//  file.close();
  InternalFS.format();
  
  //we don't need the 32K Pin, so disable it
  rtc.disable32K();
  rtc.disableAlarm(1);
  rtc.disableAlarm(2); 
  led_signal3();
  
}

void runSampling() {
  led_signal3();
  sample();
  if (!writeSample()) {
    stopSampling();
    return;
  }
  uint64_t freeBytes = SPIFlash_free();
  if (freeBytes < MIN_SPI_FLASH_FREE_BYTES) {
      stopSampling();
      return;
    } 
    
  samplingConfig config = readConfig();
  rtc.disable32K();
  rtc.disableAlarm(2);
  rtc.clearAlarm(1);
  rtc.writeSqwPinMode(DS3231_OFF);
  if (!rtc.setAlarm1(rtc.now() + TimeSpan(0, 0, 0, config.samplingIntervalSecs), DS3231_A1_Second)) { 
    Serial.println("Failed to start sampling !");
  } else {
//    NRF_POWER->GPREGRET = 0x6d;
    systemOff(RTC_INTERRUPT_PIN, 0);
  }
}


 

bool amISampling() {
    
    Adafruit_LittleFS_Namespace::File file(InternalFS);
    file.open(SAMPLE_FLAG_FILENAME, Adafruit_LittleFS_Namespace::FILE_O_READ);
    samplingFlag = 0;
    if (file) {
      file.close();
      return true;
     }
    return false;
    
}

void onAlarm() {
}

void printStatus() {
  bool usbConnected = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
  if (usbConnected ) {
    Serial.println("  USB VBUS");
  }
  if (amISampling() ) {
    Serial.println("  Sampling ON!");
  } else {
    Serial.println("  Sampling OFF!");
  }

  
  printRTCtime();
  displayVBAT();
  displayNrfxTemp();
  RTCStatus();
  uint64_t freeBytes = SPIFlash_free();
  Serial.print("  SPIFlash free capacity left: ");
  Serial.print((uint32_t)(freeBytes / 1024));
  Serial.println(" KB");
}

void printSPIflashTreeDir() {
  // You can open a directory to list all the children (files and directories).
  // Just like the SD library the File type represents either a file or
  // directory.
  File32 dataDir = fatfs.open("/");
  if (!dataDir) {
    Serial.println(F("  Error, failed to open data directory!"));
    while (1) {
      yield();
    }
  }
  if (!dataDir.isDirectory()) {
    Serial.println(F("Error, expected data to be a directory!"));
    while (1) {
      yield();
    }
  }
  Serial.println(F("Listing children of directory / :"));
  File32 child = dataDir.openNextFile();
  while (child) {
    char filename[64];
    child.getName(filename, sizeof(filename));

    // Print the file name and mention if it's a directory.
    Serial.print(F("- "));
    Serial.print(filename);
    if (child.isDirectory()) {
      Serial.print(F(" (directory)"));
    }
    Serial.println();
    // Keep calling openNextFile to get a new file.
    // When you're done enumerating files an unopened one will
    // be returned (i.e. testing it for true/false like at the
    // top of this while loop will fail).
    child = dataDir.openNextFile();
  }
}

void setup() {

  
  bool flash_ok = flash.begin(); 
  bool rtc_ok = rtc.begin();
  bool flash_mount_ok = fatfs.begin(&flash);

  if (rtc_ok) {
    pinMode(RTC_INTERRUPT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(RTC_INTERRUPT_PIN), onAlarm, FALLING);

    rtc.clearAlarm(1);
    rtc.clearAlarm(2);
  
    // stop oscillating signals at SQW Pin
    // otherwise setAlarm1 will fail
    rtc.writeSqwPinMode(DS3231_OFF);
  
    // turn off alarm 2 (in case it isn't off already)
    // again, this isn't done at reboot, so a previously set alarm could easily go overlooked
    rtc.disableAlarm(2);
    
  }

  
  // Set disk vendor id, product id and revision with string up to 8, 16, 4 characters respectively
  usb_msc.setID("Adafruit", "External Flash", "1.0");

  // Set callback
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

  // Set disk size, block size should be 512 regardless of spi flash page size
  usb_msc.setCapacity(flash.size()/512, 512);

  // MSC is ready for read/write
  usb_msc.setUnitReady(true);

  usb_msc.begin();

  bool stopped = false;
    
   // Initialize Internal File System
  InternalFS.begin();

  // i2c bus (address optional for main)
  wi->begin();        

  // nRF52840 internal temp sensor
  nrfx_temp_init(&config, NULL);

  if (amISampling() && rtc.lostPower()) {    
    stopSampling();
  }




  if (USBDevice.mounted()) {
    // USB connected to a host
    Serial.begin(115200);
    while ( !Serial ) delay(10);   // for nrf52840 with native usb
    Serial.println("  Serial connected!");
   
    if (stopped) {
      Serial.println("  Stopped sampling !");
    }

    if (amISampling()) {
      Serial.println("  I should be sampling... !");
    }

    if (!flash_ok) {
      Serial.println("  Error, failed to initialize flash chip!");
      while (1) {}
    }

    if (!rtc_ok) {
      Serial.println("  Couldn't find RTC");
      Serial.flush();
      while (1) delay(10);
    } else {
      Serial.println("  RTC OK");
    }

    if (rtc.lostPower()) {
      Serial.println("  RTC lost power, let's set the time!");
      // When time needs to be set on a new device, or after a power loss, the
      // following line sets the RTC to the date & time this sketch was compiled
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      // This line sets the RTC with an explicit date & time, for example to set
      // January 21, 2014 at 3am you would call:
      // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
    }

    
    
    if (!flash_mount_ok) {
      Serial.println(F("  Error, failed to mount newly formatted filesystem!"));
      Serial.println(F("  Was the flash chip formatted with the SdFat_format example?"));
    }
    
    Serial.println(F("  Mounted SPIFlash filesystem!"));

    Serial.println("  USB mounted!");

    Serial.print("  Flash chip JEDEC ID: 0x");
    Serial.println(flash.getJEDECID(), HEX);
    Serial.print(F("  Flash size: "));
    Serial.print(flash.size() / 1024);
    Serial.println(F(" KB"));
    printStatus();
    
    Serial.println();
   } 
      
}


void loop() {
  bool usbConnected = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
  if (usbConnected) {
    bool exitLoop = false;
    while (!exitLoop) {
      exitLoop = Menu();
    }
    Serial.println("   Exiting menu!");
  }

  if (amISampling()) {
      Serial.println("   Running sampling!");
      runSampling();  
    }
    
 
}


extern "C" {

DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
  (void)pdrv;
  return 0;
}

DRESULT disk_read(BYTE pdrv,  /* Physical drive nmuber to identify the drive */
                  BYTE *buff, /* Data buffer to store read data */
                  DWORD sector, /* Start sector in LBA */
                  UINT count    /* Number of sectors to read */
) {
  (void)pdrv;
  return flash.readBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, /* Physical drive nmuber to identify the drive */
                   const BYTE *buff, /* Data to be written */
                   DWORD sector,     /* Start sector in LBA */
                   UINT count        /* Number of sectors to write */
) {
  (void)pdrv;
  return flash.writeBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, /* Physical drive nmuber (0..) */
                   BYTE cmd,  /* Control code */
                   void *buff /* Buffer to send/receive control data */
) {
  (void)pdrv;

  switch (cmd) {
  case CTRL_SYNC:
    flash.syncBlocks();
    return RES_OK;

  case GET_SECTOR_COUNT:
    *((DWORD *)buff) = flash.size() / 512;
    return RES_OK;

  case GET_SECTOR_SIZE:
    *((WORD *)buff) = 512;
    return RES_OK;

  case GET_BLOCK_SIZE:
    *((DWORD *)buff) = 8; // erase block size in units of sector size
    return RES_OK;

  default:
    return RES_PARERR;
  }
}
}



// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and 
// return number of copied bytes (must be multiple of block size) 
int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize) {
  // Note: SPIFLash Block API: readBlocks/writeBlocks/syncBlocks
  // already include 4K sector caching internally. We don't need to cache it, yahhhh!!
  return flash.readBlocks(lba, (uint8_t*) buffer, bufsize/512) ? bufsize : -1;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and 
// return number of written bytes (must be multiple of block size)
int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, HIGH);
#endif

  // Note: SPIFLash Block API: readBlocks/writeBlocks/syncBlocks
  // already include 4K sector caching internally. We don't need to cache it, yahhhh!!
  return flash.writeBlocks(lba, buffer, bufsize/512) ? bufsize : -1;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
void msc_flush_cb (void) {
  // sync with flash
  flash.syncBlocks();

  // clear file system's cache to force refresh
  fatfs.cacheClear();

  fs_changed = true;

#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, LOW);
#endif
}
