#include <SPI.h>
#include <Wire.h>
#include <Arduino.h>
#include <Adafruit_SPIFlash.h>
#include <flash_devices.h>
#include <Adafruit_FlashTransport.h>
#include <Adafruit_FlashCache.h>
#include <InternalFileSystem.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <nrfx_temp.h>
#include <Adafruit_NeoPixel.h>
#include "RTClib.h"
#include "SdFat_Adafruit_Fork.h"
#include "Adafruit_TinyUSB.h"
// Since SdFat doesn't fully support FAT12 such as format a new flash
// We will use Elm Cham's fatfs f_mkfs() to format
#include "ff.h"
#include "diskio.h"
#include <nrf.h>


#if defined ARDUINO_NRF52840_CIRCUITPLAY
#define  PIN_VBAT          A8   // this is just a mock read, we'll use the light sensor, so we can run the test
#endif

uint32_t vbat_pin = PIN_VBAT;             // A7 for feather nRF52832, A6 for nRF52840

#define VBAT_MV_PER_LSB   (0.73242188F)   // 3.0V ADC range and 12-bit ADC resolution = 3000mV/4096

#ifdef NRF52840_XXAA
#define VBAT_DIVIDER      (0.5F)          // 150K + 150K voltage divider on VBAT
#define VBAT_DIVIDER_COMP (2.0F)          // Compensation factor for the VBAT divider
#else
#define VBAT_DIVIDER      (0.71275837F)   // 2M + 0.806M voltage divider on VBAT = (2M / (0.806M + 2M))
#define VBAT_DIVIDER_COMP (1.403F)        // Compensation factor for the VBAT divider
#endif

#define REAL_VBAT_MV_PER_LSB (VBAT_DIVIDER_COMP * VBAT_MV_PER_LSB)

#define RTC_INTERRUPT_PIN 10                //



// for flashTransport definition
#include "flash_config.h"

#define MAX_LEVEL   3
#define CLOCK_INTERRUPT_PIN 2

#define DISK_LABEL "DATA"                   // up to 11 characters
#define DATA_FILENAME "/data"               // the file where data will be stored
#define SAMPLING_CFG_FILENAME    "/config"  // the file where config will be stored

nrfx_temp_config_t config = NRFX_TEMP_DEFAULT_CONFIG;

TwoWire *wi = &Wire;

Adafruit_SPIFlash flash(&flashTransport);

FatVolume fatfs; // file system object from SdFat

// USB Mass Storage object
Adafruit_USBD_MSC usb_msc;

// Check if flash is formatted
bool fs_formatted = false;

// Set to true when PC write to flash
bool fs_changed = true;;


RTC_DS3231 rtc;

float sampleTemp = -1;
long sampleTime = -1.0;
float sampleVbat = -1.0;


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


void printStatus() {
  
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


void setSamplingConfig() {
   
   int n = SamplingIntervalMenu();
   Serial.print(F("  Setting interval to " ));
   Serial.print(n);
   Serial.println(F(" seconds" ));
   File32 writeFile = fatfs.open(SAMPLING_CFG_FILENAME, FILE_WRITE);
   if (!writeFile) {
      Serial.println(F("Error, failed to open " SAMPLING_CFG_FILENAME " for writing!"));
      while (1) {
        yield();
      }
  }
  Serial.println(F("  Writing config to file " SAMPLING_CFG_FILENAME ));
  writeFile.print("interval:");
  writeFile.println(n, DEC);
  // Close the file when finished writing.
  writeFile.close();
   
}


void sample() { 
  DateTime now = rtc.now();
  sampleTemp = rtc.getTemperature();
  sampleTime = now.unixtime();
  sampleVbat = readVBAT();

  Serial.print(F("  sample UNIXTIME: " ));
  Serial.println(sampleTime);
  Serial.print(F("  sample Temperature [C]: "));
  Serial.println(sampleTemp);
  Serial.print(F("  sample Vbat [mv]: " ));
  Serial.println(sampleVbat);
}


bool readConfig() {
  
  // reads sampling parameters from SPIFlash config file
  File32 readFile = fatfs.open(SAMPLING_CFG_FILENAME, FILE_READ);
  if (!readFile) {
      return false;
  }
  while (readFile.available()) {
    char c = readFile.read();
    Serial.print(c);
  }
  readFile.close();
  return true;
}


void showSamplingConfig() {
   bool ret = readConfig();
   
}

bool writeSample() { 
  // writes sampling data to data file
  if (!fatfs.exists(DATA_FILENAME)) {
    Serial.println(F("Data file [" DATA_FILENAME "] doesn't exist - creating!"));
    File32 writeFile = fatfs.open(DATA_FILENAME, FILE_WRITE);
    if (!writeFile) {
      Serial.println(F("  Error, failed to open " DATA_FILENAME " for writing!"));
      return false;
    }
    writeFile.close();
  }
  File32 writeFile = fatfs.open(DATA_FILENAME, FILE_WRITE);
  if (!writeFile) {
    Serial.println(F("  Error, failed to open " DATA_FILENAME " for writing!"));
    return false;
  }
  Serial.println(F("  Opened file " DATA_FILENAME " for writing/appending..."));

  writeFile.print(sampleTime,DEC);
  writeFile.print(",");
  writeFile.print(sampleVbat,DEC);
  writeFile.print(",");
  writeFile.println(sampleTemp,DEC);
  writeFile.close();
  return true;
}


void oneShotSample() {
  sample(); 
  writeSample();
  uint64_t freeBytes = SPIFlash_free();
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
  9 Run sampling \n";


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
  setSamplingConfig();  
  }

void Menu() {
    Serial.println(menu_banner);
    for (;;) {
        switch (Serial.read()) {
            case '0': option0(); Serial.println(menu_banner); break;
            case '1': option1(); Serial.println(menu_banner); break;
            case '2': option2(); Serial.println(menu_banner); break;
            case '3': option3(); Serial.println(menu_banner); break;
            case '4': option4(); Serial.println(menu_banner); break;
            case '5': option5(); Serial.println(menu_banner); break;
            case '6': option6(); Serial.println(menu_banner); break;
            case '7': option7(); Serial.println(menu_banner); break;
            case '9': option9(); Serial.println(menu_banner); break;
            default: continue;  // includes the case 'no input'
        }
    }
}

const char * sampling_banner = "\n \
  Choose sampling interval: \n \
  1 30 seconds \n \
  2 60 seconds \n \
  3 120 seconds \n \
  4 300 seconds \n \
  5 600 seconds \n ";

int SamplingIntervalMenu() {
    Serial.println(sampling_banner);
    for (;;) {
        switch (Serial.read()) {
            case '1': return 30; break;
            case '2': return 60; break;
            case '3': return 120; break;
            case '4': return 300; break;
            case '5': return 600; break;
            default: continue;  // includes the case 'no input'
        }
    }
}

void setup() {

  // Initialize Internal File System
  InternalFS.begin();
  // i2c bus (address optional for main)
  wi->begin();        

  // nRF52840 internal temp sensor
  nrfx_temp_init(&config, NULL);

  bool flash_ok = flash.begin();
  bool rtc_ok = rtc.begin();
  bool usb_mounted = USBDevice.mounted();


  // Set disk vendor id, product id and revision with string up to 8, 16, 4 characters respectively
  usb_msc.setID("Adafruit", "External Flash", "1.0");

  // Set callback
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

  // Set disk size, block size should be 512 regardless of spi flash page size
  usb_msc.setCapacity(flash.size()/512, 512);

  // MSC is ready for read/write
  usb_msc.setUnitReady(true);

  usb_msc.begin();

  
  
//  if (usb_mounted) {
   if (1==1) {
    
    // USB connected to a host
    Serial.begin(115200);
    while ( !Serial ) delay(10);   // for nrf52840 with native usb
    dbgMemInfo();
    Serial.println(usb_mounted);
    
    if (!flash_ok) {
      Serial.println("Error, failed to initialize flash chip!");
      while (1) {}
    }

    if (!rtc_ok) {
      Serial.println("Couldn't find RTC");
      Serial.flush();
      while (1) delay(10);
    }

    if (rtc.lostPower()) {
      Serial.println("RTC lost power, let's set the time!");
      // When time needs to be set on a new device, or after a power loss, the
      // following line sets the RTC to the date & time this sketch was compiled
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      // This line sets the RTC with an explicit date & time, for example to set
      // January 21, 2014 at 3am you would call:
      // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
    }

    Serial.print("  Flash chip JEDEC ID: 0x");
    Serial.println(flash.getJEDECID(), HEX);
    Serial.print(F("  Flash size: "));
    Serial.print(flash.size() / 1024);
    Serial.println(F(" KB"));
    

    if (!fatfs.begin(&flash)) {
      Serial.println(F("  Error, failed to mount newly formatted filesystem!"));
      Serial.println(F("  Was the flash chip formatted with the SdFat_format example?"));
    }
    Serial.println(F("  Mounted SPIFlash filesystem!"));

    // Get a single ADC sample and throw it away
    readVBAT();
    printStatus();

    Serial.println();
   
   } else {

      // run the logger
      for(int i=0; i<4; i++) {
        digitalToggle(LED_RED); // Toggle LED 
        delay(2000);   
      }
      sample();
      writeSample();
      for(int i=0; i<6; i++) {
        digitalToggle(LED_RED); // Toggle LED 
        delay(1000);   
      }
      
    
   }
      
}


void loop() {
  
  if (USBDevice.mounted()) {
    Menu();
    delay(1000);
  } else {
    
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
