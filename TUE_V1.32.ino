//Lauffähig ab LP Stand 0.51
//Ein-taster auf GPIO16-RST und Ausfunktion am Config Schalter
//hoe 10.03.17
//hoe 1.6.17 V1.2 Mit NTP Funktion, sobald ein ntp server eingetragen wurde, wird versucht die aktuelle Zeit mit NTP abzufragen und die Uhr danach zu stellen. 
//           wird jedoch ein MRCLOCK Telegramm empfangen, wird der NTP Empfang um die Wartezeit WaittimeNTPSync unterdrückt, MRCLOCK Empfang hat vorrang vor NTP!
//           als Zeitzone wird standardmässig MEZ mit Sommerzeit eingestellt.
//hoe 15.8.17 V1.3 Abgabeversion mit Entladespannung des Akkus bei 3.1V; Start nachdem Akku 3.6V erreicht hat. Systemwechselzeit MRClock -> NTP = 24h
//hoe 30.12.17 V1.3.1 kleinere Korrekturen (Rechtschreibung)
//hoe 10.03.18 V1.3.2 längere Taktpause beim Stellen eingebaut, damit auch grössere Uhren nachkommen. (Nach test vom Treffen Mammendorf 2017) war Taktpause war 500ms, jetzt 1000ms

#define VERSION "V1.3.2"

#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <TickerScheduler.h>      //Ticker

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

//needed for library
#include <ESP8266WebServer.h> 
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

//needed for NTP 
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <Timezone.h> //by Jack Christensen, not included in the Arduino IDE !!!

//Defines for Pins

#define CHARGEPUMPENABLE 14       //SPI SCK = GPIO #14 (Charge Pump enable)
#define TAKTA            13       //SPI MOSI = GPIO #13 (TaktA)
#define TAKTB            12       //SPI MISO = GPIO #12 (TaktB)
#define CONFIGPB          5       //GPIO #5 (Input für Config Pushbutton) Bei MRC_REC = 16!
#define BLED              2       //GPIO #2 = blue LED on ESP
#define INT               4       //GPIO #4 = Interrupt für Spannungseinbruch -> Interrupt wird an der fallenden Flanke ausgelöst

//defines für Zeiten
#define TAKTTIME          10      //Wie lange wird der Stelltakt ausgegeben? 1 Einheit entspricht 10 ms Taktausgabezeit
#define TAKTWAITIME       100      //So lange darf kein Takt ausgegeben werden     
#define CHARGEPUMPTIME    2       //So lange wird auf die Chargepump gewartet
#define WaittimeNTPSync   1440000 //Zeit in ms bis von MRCLOCK Telegrammempfang auf NTP umgeschalten wird. 1440000 entspricht 24h
#define WaittimeNTP1Sync  36000   //Zeit in 10ms. Uhr stellt sich nach der Configroutine auf NTP um, sofern kein MRCLOCK Telegramm empfangen wurde. 36000 entsprechen 6 Minuten

//defines für den LED Status
#define LEDOff            0       //LED is off
#define LEDBlinking       1       //Flashing LED (Saving Time to Flash)
#define LEDAlive          2       //TUE is alive
#define LEDConfig         3       //TUE is in Config mode
#define LEDOn             5       //LED is on
#define LEDBlinkOnce      6       //LED macht einen Blinker
#define LEDAusVorbereitet 7       //LED macht langsamen Blinkrythmus

int LEDStatus = 0;                //LED Status wird durch das Programm gemäss den oben angegebenen Defines gesetzt und durch die Task LED() wird die LED angesteuert
int LEDStatusIntern = 0;          //LED Status intern für die LED Routine
int LEDStatusCounter = 0;         //LED Status Counter zum zählen bis zum nächsten Statuswechsel
int LEDPeriodCounter = 0;         //Dient als Zähler für das Aus vorbereitet....

#define LEDalivetime      1
#define LEDalivecycle     1000    //LED alive = jede 10s ein kurzer blauer blitz
#define LEDblinkingcycle  5
#define LEDAusVorbtime    2
#define LEDAusVorbcycle   5
#define LEDAusVorbperiod  1000
       
//define your default values here, if there are different values in config.json, they are overwritten.
char mrc_multicast[40] = "239.50.50.20";
char mrc_port[6] = "2000";

//char array for the protokollhandler 
char c_clock_hour[15] = "03";
char c_clock_minute[15] = "02";
char c_clock_sek[15] = "04";
char c_time[15] = "0";
char c_speed[15] = "0";

//Strings 
String esp_chipid;

//ADC Daten
int  adcwert;                   //Aktueller ADC Wert
int  adcmittelwert;             //Mittelwert
bool fESPrunning = true;        //Flag spiegelt den Zustand des ESP wieder true bedeutet genügend VCC, false = ESP ruht
bool fDataSaving = false;       //Die Daten müssen ins Flash gespeichert werden, der ESP hat zuwenig Spannung

#define adcstopgrenzwert  440         //ab diesem ADC Wert schaltet der ESP ab und zirkuliert in einer Warteschleife bis adc > adcstart 440 entspricht ca. 3.12 V
#define adcstartgrenzwert 499         //ab diesem ADC Wert schaltet der ESP wieder ein
#define adcconnectedio    300         //falls der adc beim Starten mehr als diesen Wert liest, gilt der adc als verdrahtet und wird benutzt

//Daten aus dem Protokoll
int  clock_h = 0;   // Sollzeit Stunde
int  clock_m = 0;   // Sollzeit Minute
int  clock_s = 0;   // Sollzeit Sekunde
int  clock_speed = 1; // 

int  tochter_h = 0; // Istzeit auf der 12h Tochteruhr (Stunden)
int  tochter_m = 0; // Istzeit auf der 12h Tochteruhr (Minuten)

//NTP Variablen
char ntpserver[40] = "ntp.metas.ch";         //NTPServer
unsigned int localPort = 8888;               // local port to listen for UDP packets
const int timeZone = 0;                      // UTC
double dCounterToNTP;                        //if no MRCLOCK telegramm is recieved, the conter gets decressed, on ZERO the NTP time is displayed

//Timezone
//Central European Time (Frankfurt, Paris)
TimeChangeRule CEST = { "CEST", Last, Sun, Mar, 2, 120 };     //Central European Summer Time
TimeChangeRule CET = { "CET ", Last, Sun, Oct, 3, 60 };       //Central European Standard Time
Timezone CE(CEST, CET);
TimeChangeRule *tcr;        //pointer to the time change rule, use to get the TZ abbrev
time_t utc, local;
time_t prevDisplay = 0;     // when the digital clock was displayed

//Statemaschine für TochterUhrStellen Routine
int  SM = 0;        // Statemaschine Status
int  SMC = 0;       // Statemaschine Counter
bool FlagTUStellen = false;          // Flag ist gesetzt, falls die Tochteruhr einen Tick machen soll
bool FlagTUStellglied;               // Flag wiederspiegelt den Ausgang des TU Stellglieds

//Rücksetztaste
bool FlagButtonPresed = false;
int  CounterButtonPressed;

//Tickerscheduler Objekt

TickerScheduler ts(6);

//flag for saving data
bool shouldSaveConfig = false;

//flag for sleep mode 
bool sleep = false;
bool flag_message_recieved = true;

//WifiUDP Class
WiFiUDP wifiUDPMRC;    //für MRClock Telegramme
WiFiUDP wifiUDPNTP;    //für NTP Telegramme

const int MRC_PACKET_SIZE = 2024; // NTP time stamp is in the first 48 bytes of the message

char packetBuffer[ MRC_PACKET_SIZE ]; //buffer to hold incoming and outgoing packets

int nLength = 0;

int time_hour = 0;

int time_minute = 0;

//WiFiManager
  //Das Object wird auch noch im LOOP benötigt um die Config zu löschen.... 
  WiFiManager wifiManager;
  //Die beiden Parameter werden durch ein Callback verändert, deswegen müssen sie Global definiert sein
  WiFiManagerParameter custom_time_hour();
  WiFiManagerParameter custom_time_minute();

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// CALLBACK wird mit dem Starten der Configurationsseite aufgerufen und stellt die Uhr um zwei Ticks weiter, damit sie mit der internen Uhr synchron läuft
void APCallback(WiFiManager *myWiFiManager){
  
  forcetick();
 
  while(FlagTUStellen){
    yield();
    ts.update();
    Serial.print(".");
    delay(10);
  }
  
  forcetick();
  
  while(FlagTUStellen){
    yield();
    ts.update();
    Serial.print(".");
    delay(10);
  }
  
  WiFiManagerParameter custom_time_hour("clock_hour", "zulaessige Werte (00-11)", c_clock_hour, 15);
  WiFiManagerParameter custom_time_minute("clock_minute", "zulaessige Werte (00-59)", c_clock_minute, 15);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println();

  LED(LEDOff);               // Am Anfang ist die LED aus....

  //ChipID?
  esp_chipid = String(ESP.getChipId());

  //Identify
  Serial.print("Tochteruhrempfänger TUE ");
  Serial.print(VERSION);
  Serial.print(" ESP - ChipID = ");
  Serial.println(esp_chipid); 

  Serial.println("Configuring Pins and Interrupts");

  //WiFi.setAutoReconnect(false);
  WiFi.persistent(true); // Wird gebraucht damit das Flash nicht zu oft beschrieben wird.
  WiFi.setAutoReconnect(true);

  //Pins konfigurieren
  //SPI SCK = GPIO #14 (Charge Pump enable)
  //SPI MOSI = GPIO #13 (TaktA)
  //SPI MISO = GPIO #12 (TaktB)
  //GPIO #5 (Input für Config Pushbutton)
  //GPIO #16 for Enable and Battery Saving :-)
  //GPIO #2 = blue LED on ESP
  //GPIO #9 = Überwachung der Versorgungsspannung

  pinMode(TAKTA, OUTPUT);
  pinMode(TAKTB, OUTPUT);
  pinMode(CHARGEPUMPENABLE, OUTPUT);
  pinMode(CONFIGPB, INPUT);
  pinMode(BLED, OUTPUT);
  
  digitalWrite(CHARGEPUMPENABLE, HIGH); 
  digitalWrite(BLED, HIGH);
  
  ts.add(4, 10, LEDTS);

  LED(LEDBlinkOnce);         // LED einen Flash blinken -> 1. Blinkimpuls

  //clean FS, for testing
  //SPIFFS.format();
  //SPIFFS.remove("/config.json");
  //SPIFFS.remove("/config2.json");
  
  //read configuration from FS json
  Serial.println("mounting FS...");

  // Falls das Filesystem noch nicht formatiert ist, wird es hier automatisch gemacht.
  if(SPIFFS.begin()){
    Serial.println("FS ok!");
  } else {
    SPIFFS.format();
    Serial.println("FS formated");
  }

  LED(LEDBlinkOnce);     // LED einen Flash blinken  -> 2. Blinkimpuls
  
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          strcpy(ntpserver, json["ntp_server"]);
          strcpy(mrc_multicast, json["mrc_multicast"]);
          strcpy(mrc_port, json["mrc_port"]);
          strcpy(c_clock_hour, json["clock_hour"]);
          strcpy(c_clock_minute, json["clock_minute"]);
          if(json["FlagTUStellglied"] == "true"){
            FlagTUStellglied = true;
            digitalWrite(TAKTA, LOW);
            digitalWrite(TAKTB, LOW);
            Serial.print("FlagTUStellglied = true");
          }else{
            FlagTUStellglied = false;
            digitalWrite(TAKTA, HIGH);
            digitalWrite(TAKTB, HIGH); 
            Serial.print("FlagTUStellglied = false");
          }
        } 
      }
    }
  } else {
    Serial.println("failed to mount FS");
    FlagTUStellglied = true;
    digitalWrite(TAKTA, LOW);
    digitalWrite(TAKTB, LOW);
    Serial.println("failed to load json config");
  }
  //end read

  //Interne Zeit mit der Tochteruhrzeit gleich setzen, sonst läuft uns die Uhr nach einem Neustart los...
  tochter_h = atoi(c_clock_hour);
  tochter_m = atoi(c_clock_minute);

  sprintf(c_clock_hour, "%i", tochter_h);
  sprintf(c_clock_minute, "%i", tochter_m);

  Serial.print("Tochteruhr zeigt ");
  Serial.print(tochter_h);
  Serial.print(":");
  Serial.print(tochter_m);
  Serial.println(" an.");

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_text_info1("<br>Einstellmen&uuml; f&uuml;r den Tochteruhrempf&auml;nger.<br>");
  WiFiManagerParameter custom_text_info2("Empf&auml;ngt Zeitzeichentelegramme nach dem MRClock Protokoll.<br><br>");
  WiFiManagerParameter custom_text_info3("Standarteinstellung MRClock: Multicast 239.50.50.20 auf Port 2000.<br>Bitte nur die Stellung der Nebenuhr anpassen. Die anderen Einstellungen m&uuml;ssen meistens nicht ge&auml;ndert werden.<br>");
  WiFiManagerParameter custom_text_h("Stand der Nebenuhr, hier Stunden (ganze Zahl von 0 bis 11):");
  WiFiManagerParameter custom_text_m("Stand der Nebenuhr, hier Minuten (ganze Zahl von 0 bis 59):");
  WiFiManagerParameter custom_text_expert("<br>Ab hier Experteneinstellungen, nur &auml;ndern wenn wann weiss was man macht!<br>");
  WiFiManagerParameter custom_text_multicast_adress("Empfangsadresse der MRClock Telegramme:");
  WiFiManagerParameter custom_mrc_multicast("mrc_multicast", "mrc_multicast (239.50.50.20)", mrc_multicast, 40);
  WiFiManagerParameter custom_text_multicast_port("Empfangsport der MRClock Telegramme:");
  WiFiManagerParameter custom_mrc_port("mrc_port", "mrc_multicast_port (2000)", mrc_port, 5);
  WiFiManagerParameter custom_text_ntpserver("<br>Zeitserver zum Synchronisieren auf die MEZ:");
  WiFiManagerParameter custom_ntp_server("NTP_Server", "ntp.server.de", ntpserver, 40);
  WiFiManagerParameter custom_time_hour("clock_hour", "zul&auml;ssige Werte (00-11)", c_clock_hour, 15);
  WiFiManagerParameter custom_time_minute("clock_minute", "zul&auml;ssige Werte (00-59)", c_clock_minute, 15);

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set APCallback
  wifiManager.setAPCallback(APCallback);

  //set static ip, otherwise it will use dhcp...
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
  
  //add all your parameters here
  wifiManager.addParameter(&custom_text_info1);
  wifiManager.addParameter(&custom_text_info2);
  wifiManager.addParameter(&custom_text_info3);
  wifiManager.addParameter(&custom_text_h);
  wifiManager.addParameter(&custom_time_hour);
  wifiManager.addParameter(&custom_text_m);
  wifiManager.addParameter(&custom_time_minute);
  wifiManager.addParameter(&custom_text_expert);
  wifiManager.addParameter(&custom_text_ntpserver);
  wifiManager.addParameter(&custom_ntp_server);  
  wifiManager.addParameter(&custom_text_multicast_adress);
  wifiManager.addParameter(&custom_mrc_multicast);
  wifiManager.addParameter(&custom_text_multicast_port);
  wifiManager.addParameter(&custom_mrc_port);

  //reset settings - durch das löschen der Daten, haben wir die Möglichkeit die Uhrzeit bei jedem Start neu einstellen zu können
  //wifiManager.resetSettings();

  //set minimum quality of signal so it ignores AP's under that quality
  //defaults to 8%
  //wifiManager.setMinimumSignalQuality();
  
  //sets timeout until configuration portal gets turned off
  //useful to make it all retry or go to sleep
  //in seconds
  wifiManager.setTimeout(600);

  LED(LEDBlinkOnce);         // LED einen Flash blinken    -> 3. Blinkimpuls

  LED(LEDOn);                // Ab jetzt ist die LED an, falls das CAPTIVE PORTAL gestartet ist
                             // Ansonsten 4. Blinkimpuls (lang)

  // Setze Istzeit = Sollzeit, sonst läuft die Uhr schon nach dem Setzen los auf irgendeine Zeit...
  clock_h = tochter_h;
  clock_m = tochter_m;

  //Init ADC
  adcwert = analogRead(A0);   //aktuellen ADC Wert lesen
  delay(100);                 //100ms warten
  adcmittelwert = (adcwert + analogRead(A0))/2;
  delay(100);                 //100ms warten

  Serial.print("ADC Level = ");
  Serial.print(adcwert);
  Serial.print(" ADC Mittelwert = ");
  Serial.println(adcmittelwert);
  
  ts.add(0, 10, tick);
  ts.add(2, 10, TochterUhrStellen);
  ts.add(3, 1000, TochterUhr);
  
  if(adcmittelwert > adcconnectedio){    //ADC Wird verwendet
    Serial.println("ADC verdrahtet wird verwendet");
    ts.add(1, 60000, UBat);
    fESPrunning = true;
    if(ts.enable(1)){
      Serial.println("TASK 1, UBat() enabled");
    }
    if(adcmittelwert < adcstopgrenzwert){
      fESPrunning = false;
      Serial.print("ADC: ESP ruht");
      LEDStatus = LEDOff;                  //LED aus
      ts.disable(0);
      ts.disable(2);
      ts.disable(3);
    }
  }
  else Serial.println("ADC funktioniert nicht, wird nicht verwendet!");

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration //"AutoConnectAP", "password1234"
  if (!wifiManager.autoConnect(String("TUE" + esp_chipid).c_str())) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    //ESP.reset();
    //wifi_set_sleep_type(LIGHT_SLEEP_T); 
    system_deep_sleep(1000000);
    //ESP.deepSleep(1000000);
    delay(100);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  
  //read updated parameters
  strcpy(ntpserver, custom_ntp_server.getValue());
  strcpy(mrc_multicast, custom_mrc_multicast.getValue());
  strcpy(mrc_port, custom_mrc_port.getValue());
  strcpy(c_clock_hour, custom_time_hour.getValue());
  strcpy(c_clock_minute, custom_time_minute.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");

    tochter_h = atoi(c_clock_hour);
    tochter_m = atoi(c_clock_minute);

    //Parameter überprüfen
    if(tochter_h < 0 || tochter_h > 23){
      tochter_h = 0;
    }
    if(tochter_h >= 12){
      tochter_h = tochter_h - 12;
    }
    if(tochter_m < 0 || tochter_h > 59){
      tochter_m = 0;
    }
    
    // Setze Sollzeit = Istzeit, sonst läuft die Uhr schon nach dem Setzen los auf irgendeine Zeit...
    clock_h = tochter_h;
    clock_m = tochter_m;
    
    Serial.print("Uhrzeit der Tochteruhr=");
    Serial.print(tochter_h);
    Serial.print(":");
    Serial.println(tochter_m);

    DataSaving();
  }

  LED(LEDOff);           // LED aus machen

  Serial.println("/nlocal ip");
  Serial.println(WiFi.localIP());
  Serial.println(WiFi.SSID());
  
  //saving credentials only on change
  //WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  
  // Starting WIFI in Station Mode
  Serial.println("Starting WIFI in Station Mode, Listening to UDP Multicast");
  wifiUDPMRC;
  wifiUDPNTP;
  
  WiFi.mode(WIFI_STA);

  WiFi.printDiag(Serial);

  wifiUDPMRC.beginMulticast(WiFi.localIP(), IPAddress(239,50,50,20), 2000);
  wifiUDPNTP.begin(localPort);

  Serial.print("Local port: ");
  Serial.println(wifiUDPNTP.localPort());
  Serial.println("waiting for sync");
  setSyncProvider(getNtpTime);
  setSyncInterval(300);
  delay(5000);

  dCounterToNTP = WaittimeNTP1Sync;        // Wie lange nach dem Aufstarten warten, bis die NTP Zeit übernommen wird?

  LED(LEDBlinkOnce);         // LED einen Flash blinken    -> 4. Blinkimpuls

  LED(LEDAlive);             // Ab jetzt zeigt die LED das Leben des Empfängers an..
}

void loop() {
  // loop checks for:
  // - MRCLOCK Telegramm, or
  // - NTP 
  // MRCLOCK has priority to NTP telegramms, if there were no MRCLOCK Telegramm for WaittimeNTPSync [seconds] then, the 
  // Slaveclock is syncronised to NTP Clock until the first MRCLOCK telegramm comes in...
  // If a MRCLOCK Telegramm is recieved:
  if (int packetsize = wifiUDPMRC.parsePacket()) {
    dCounterToNTP = WaittimeNTPSync;           // Counter updaten
    Serial.println("@.");
    // We've received a packet, read the data from it
    wifiUDPMRC.read(packetBuffer, packetsize); // read the packet into the buffer

    //Search for Message like "clock=10:32:35"
    for(int i=0; i < sizeof(packetBuffer); i++){
       if( packetBuffer[i] == 'c' &&
           packetBuffer[i+1] == 'l' &&
           packetBuffer[i+2] == 'o' &&
           packetBuffer[i+3] == 'c' &&
           packetBuffer[i+4] == 'k' &&
           packetBuffer[i+5] == '='){
            c_time[0] = packetBuffer[i+6];
            c_time[1] = packetBuffer[i+7];
            c_time[2] = packetBuffer[i+8];
            c_time[3] = packetBuffer[i+9];
            c_time[4] = packetBuffer[i+10];
            c_time[5] = packetBuffer[i+11];
            c_time[6] = packetBuffer[i+12];
            c_time[7] = packetBuffer[i+13];
            c_time[8] = packetBuffer[i+14];
            
            // Time Char Array zerlegen
            bool clear_h = true;
            bool clear_m = false;
            bool clear_s = false;
            
            for(int ii=0; ii < 15; ii++){
              if( !clear_h && !clear_m && clear_s){                
                if( c_time[ii] == 0){
                  clear_m = false;
                  clear_s = false;
                  c_clock_hour[ii] = ' ';
                  c_clock_minute[ii] = ' ';
                  c_clock_sek[ii] = ' ';
                }else{
                  c_clock_hour[ii] = ' ';
                  c_clock_minute[ii] = ' ';
                  c_clock_sek[ii] = c_time[ii];
                }
              }
              if( !clear_h && clear_m && !clear_s){                
                if( c_time[ii] == ':'){
                  clear_m = false;
                  clear_s = true;
                  c_clock_hour[ii] = ' ';
                  c_clock_minute[ii] = ' ';
                  c_clock_sek[ii] = ' ';
                }else{
                  c_clock_hour[ii] = ' ';
                  c_clock_minute[ii] = c_time[ii];                  
                  c_clock_sek[ii] = ' ';
                }
              }
              if( clear_h && !clear_m && !clear_s){                
                if( c_time[ii] == ':'){
                  clear_h = false;
                  clear_m = true;
                  c_clock_hour[ii] = ' ';
                  c_clock_minute[ii] = ' ';
                  c_clock_sek[ii] = ' ';
                }else{
                  c_clock_hour[ii] = c_time[ii];
                  c_clock_minute[ii] = ' ';
                  c_clock_sek[ii] = ' ';
                }
              }
            }
  
            clock_h = atoi(c_clock_hour);
            clock_m = atoi(c_clock_minute);
            clock_s = atoi(c_clock_sek);

            Serial.print("Empfangenes Zeitzeichen:");
            Serial.print(clock_h);
            Serial.print(":");
            Serial.print(clock_m);
            Serial.print(":");
            Serial.println(clock_s);
         }
     }
     //Search for Message like "speed=5"
     for(int i=0; i < sizeof(packetBuffer); i++){
       if( packetBuffer[i] == 's' &&
           packetBuffer[i+1] == 'p' &&
           packetBuffer[i+2] == 'e' &&
           packetBuffer[i+3] == 'e' &&
           packetBuffer[i+4] == 'd' &&
           packetBuffer[i+5] == '='){
            if(packetBuffer[i+6] != 32){
              c_speed[0] = packetBuffer[i+6];
            }
            if(packetBuffer[i+7] != 32){
              c_speed[1] = packetBuffer[i+7];
            }
            if(packetBuffer[i+8] != 32){
              c_speed[2] = packetBuffer[i+8];
            }
            if(packetBuffer[i+9] != 32){
              c_speed[3] = packetBuffer[i+9];
            }
            c_speed[4] == NULL;

            Serial.println("Speed=");
            Serial.println(c_speed);
  
            clock_speed = atoi(c_speed);
            
            Serial.println(clock_speed);
         }
      }

      // recieving one message!
      Serial.println("; Ende der Message");
      flag_message_recieved = true;
  }
  

  // Ist der Taster gedrückt?
  ConfigButton();

  // Haben wir noch Saft?
  if(fDataSaving){
    DataSaving();
    fDataSaving = false;
  }
  
  while(!fESPrunning){    //While schleife solange wir keine Betriebsspannung haben
    //Tasks bedienen
    LEDStatus = LEDOff;   //LED aus
    yield();
    ts.update();
    Serial.print("s");
    LEDTS();
    delay(1000);

    // Haben wir noch Saft?
    if(fDataSaving){
      DataSaving();
      fDataSaving = false;
    }
    
    // ChargePump abschalten
    digitalWrite(CHARGEPUMPENABLE, HIGH);
    //ESP abschalten   
    ESP.deepSleep(10);
    delay(1000);
  } 

  //Soll auf NTP-Zeitzeichenempfang umgeschaltet werden?
  if(dCounterToNTP == 0){
    //jetzt wird die NTP Zeit an der Tochteruhr angezeigt...
    if (timeStatus() != timeNotSet) {
      if (now() != prevDisplay) { //update the display only if time has changed
        prevDisplay = now();

        local = CE.toLocal(now(), &tcr);
        
        Serial.print("Hour=");
        Serial.print(hour(local), DEC);
        Serial.print("Minute=");
        Serial.print(minute(local), DEC);
        Serial.print("Seconds=");
        Serial.println(second(local), DEC);
  
        clock_h = hour(local);
        clock_m = minute(local);
        clock_s = second(local);
      }
    }
  }else dCounterToNTP--;

  //Tasks bedienen
  yield();
  ts.update();
  Serial.print(".");
  delay(10);
}

//Task zum weiterstellen der Tochteruhr
//Wird zyklisch aufgerufen und prüft, ob die empfangene Uhrzeit mit der angezeigten Uhrzeit übereinstimmt. 
//Stellt sie einen Unterschied fest, setzt sie das Flag "FlagTUStellen", dieses Flag wird durch die Task "TochterUhrStellen" 
//ausgewertet, bearbeitet und zurückgestellt.
//Task verwaltet die Uhrzeit der Tochteruhr.
void * tick(){  
  if( !FlagTUStellen ){
    // Zeitzeichen mit Tochteruhr vergleichen, die Tochteruhr ist eine 12h Uhr. Das Zeitzeichen kann auch im 24h gesendet werden.
    if( ( clock_h == tochter_h && clock_m == tochter_m ) || ( clock_h-12 == tochter_h && clock_m == tochter_m ) ){
      // Tochteruhr stimmt mit Zeitzeichen überein, nix machen
    } else {
      // Tochteruhr muss gestellt werden.
      tochter_m++;
      if (tochter_m == 60){ // Stundensprung
        tochter_h++;
        tochter_m = 0;
        if (tochter_h == 12){ // Tagesprung
          tochter_h = 0;
        }
      }
      FlagTUStellen = true;
      Serial.print("Tochteruhr stellt auf Zeit=");
      Serial.print(tochter_h);
      Serial.print(":");
      Serial.print(tochter_m);
      Serial.print(" Zeitzeichen=");
      Serial.print(clock_h);
      Serial.print(":");
      Serial.print(clock_m);
      Serial.print(":");
      Serial.println(clock_s);
    }
  }
}

//Task zum gezwungenen Weiterstellen der Tochteruhr
//Wird durch das Captive Portal aufgerufen und stellt sicher, dass die angeschlossene Tochteruhr 
//synchron zum internen Stellglied läuft.
//FlagTUStellen muss false sein, beim Aufruf der Routine, sonst keine Auswirkung.
void * forcetick(){  
  if( !FlagTUStellen ){
    // Tochteruhr muss gestellt werden.
    tochter_m++;
    if (tochter_m == 60){ // Stundensprung
      tochter_h++;
      tochter_m = 0;
      if (tochter_h == 12){ // Tagesprung
        tochter_h = 0;
      }
    }      
    FlagTUStellen = true;
    Serial.print("Tochteruhr stellt auf Zeit=");
    Serial.print(tochter_h);
    Serial.print(":");
    Serial.print(tochter_m);
    Serial.print(" Zeitzeichen=");
    Serial.print(clock_h);
    Serial.print(":");
    Serial.print(clock_m);
    Serial.print(":");
    Serial.println(clock_s);
  }
}


void * TochterUhr(){
  Serial.print("Tochteruhr zeigt=");
  Serial.print(tochter_h);
  Serial.print(" : ");
  Serial.print(tochter_m);
  Serial.println(" ");
}

//Task gibt den Stelltakt an die Tochteruhr raus. Sobald das Flag "FlagTUStellen" gesetzt ist, wird ein Stellbefehl ausgegeben. 
//Das Flag wird nach der Ausgabe des Stellbefehls und dem Abschalten des Ausgangs wieder zurückgesetzt.
//Abarbeitung nach einer Statemaschine. Somit kann die Zeitdauer des Ausgangsimpuls kontrolliert werden.
//Sobald ein Takt ausgegeben werden soll, wird zuerst die Ladungspumpe eingeschaltet, nachher der Takt ausgeben, die 
//Ausgänge abgeschaltet und die Ladungspumpe wieder abgeschaltet, eher ein neuer Stelltakt ausgegeben werden kann.
void * TochterUhrStellen(){
  if( SM < 7 && SM > 0){
    if(SM == 1){
      // Warten auf FlagTUstellen
      if( FlagTUStellen ){
        SM = 2;
        Serial.print("FlagTUStellen=");
        Serial.print("true");
      }   
    }
    if(SM == 2){
      // State Chargepump init
      digitalWrite(CHARGEPUMPENABLE, LOW);
      SM = 3;
      SMC = CHARGEPUMPTIME;
    }
    if(SM == 3){
      // Warten auf Spannung
      if( SMC == 0 ){
        SM = 4;
        SMC = TAKTTIME; 
        // Tick ausgeben, Ausgänge anschalten
        if(FlagTUStellglied){
          digitalWrite(TAKTA, LOW);
          digitalWrite(TAKTB, HIGH);
          Serial.println("FlagTUStellglied = +");
          FlagTUStellglied = false;
        }else{
          digitalWrite(TAKTA, HIGH);
          digitalWrite(TAKTB, LOW);
          Serial.println("FlagTUStellglied = -");
          FlagTUStellglied = true;
        }
      }else{
        SMC--;
      }
    }
    if(SM == 4){
      // Tick ausgeben
      if( SMC == 0 ){
        SM = 5;      
        //Tick zurückstellen
        if(!FlagTUStellglied){
          digitalWrite(TAKTA, HIGH);
          digitalWrite(TAKTB, HIGH);
          Serial.println("FlagTUStellglied = -");
        }else{
          digitalWrite(TAKTA, LOW);
          digitalWrite(TAKTB, LOW);
          Serial.println("FlagTUStellglied = +");
        }
      }else{
        SMC--;
      }
    }
    if(SM == 5){
      // ChargePump abschalten
      digitalWrite(CHARGEPUMPENABLE, HIGH);
      SM = 6 ;  
      SMC = TAKTWAITIME;
    }
    if(SM == 6){
      // Warte State nach Stelltaktausgabe
      if( SMC == 0 ){
        SM = 1;
        SMC = 0;
        FlagTUStellen = false;
      }else{
        SMC--;
      }
    }
  } else { // Init Statemaschine
    SM = 1;
    SMC = 0;
  } 
}

void DataSaving(void){
  Serial.println("Saving Data to File...");
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["ntp_server"] = ntpserver;
  json["mrc_multicast"] = mrc_multicast;
  json["mrc_port"] = mrc_port;
  json["clock_hour"] = tochter_h;
  json["clock_minute"] = tochter_m;
  json["FlagTUStellglied"] = FlagTUStellglied;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
     Serial.println("failed to open config file for writing");
  }

  json.printTo(Serial);
  json.printTo(configFile);
  configFile.close();
}

void ConfigButton(void){
  if(digitalRead(CONFIGPB) == LOW){
    if(LEDStatus == LEDAusVorbereitet){ //ESP in den Deep-Sleep versetzen
      while(FlagTUStellen){             //Prüfen, ob gerade eine Taktausgabe erfolgt, wenn ja dann darauf warten...
        //Tasks bedienen
        yield();
        ts.update();
        Serial.print(".");
        delay(10);
      }
      DataSaving();
      //wifi_set_sleep_type(LIGHT_SLEEP_T); 
      system_deep_sleep(1000000);
      //ESP.deepSleep(1000000);
      delay(100);
    }
    if(FlagButtonPresed){
      if(CounterButtonPressed == 0){
        ts.remove(0);
        while(digitalRead(CONFIGPB) == LOW){
          yield();
          ts.update();
          Serial.print(".");
          delay(10); 
          LEDStatus = LEDBlinking;      // LED blinken lassen
        }
        
        // jetzt Config im Flash löschen
        Serial.println("erasing");
        Serial.printf("SPIFFS.remove = %d", SPIFFS.remove("/config.json"));
        Serial.print(".");

        WiFi.persistent(true);
        
        delay(1000);
        WiFi.setAutoReconnect(false);
        delay(100);
        WiFi.disconnect();
        delay(100);
        wifiManager.resetSettings();
        ESP.eraseConfig();
        delay(100);

        Serial.print("..ESP Reset..");
        delay(1000);
        ESP.reset();
        //ESP.restart();
        delay(5000);
      } else CounterButtonPressed--;
    }else{
      FlagButtonPresed = true;
      CounterButtonPressed = 300;
      Serial.println("BP");

      LEDStatus = LEDOn;        //Blaue LED anschalten
    }
  }else{
    // Taste wird vor dem Ablaufen des CounterButtonPressed losgelassen, dann nur Daten ins Flash sichern
    if(FlagButtonPresed){
      FlagButtonPresed = false;
      CounterButtonPressed = 0;
      //LEDStatus = LEDAlive;       //Blaue LED zeigt Lebenszeichen

      Serial.println("Saving Data to File...");

      DataSaving();
      
      ts.add(0, 10, tick);
      
      LEDStatus = LEDAusVorbereitet;  //Jetzt kann man den TUE ausschalten
    }
    
  }
}

void * LEDTS(void){
  if(LEDStatusIntern != LEDStatus){           //LED Status hat sich verändert, alle Counter reseten
    LEDStatusCounter = 0;
    LEDStatusIntern = LEDStatus;
    if(LEDStatusIntern == LEDAusVorbereitet) LEDPeriodCounter = LEDAusVorbperiod;
  }
  if(LEDStatusIntern == LEDOff){
    digitalWrite(BLED,  HIGH);    
  }
  if(LEDStatusIntern == LEDOn){
    digitalWrite(BLED,  LOW);
  }
  if(LEDStatusIntern == LEDBlinking){
    if(!LEDStatusCounter){
      LEDStatusCounter = LEDblinkingcycle;
      if(digitalRead(BLED)) digitalWrite(BLED, LOW);
      else digitalWrite(BLED, HIGH);
    }
    else LEDStatusCounter--;
  }
  if(LEDStatusIntern == LEDAlive){
    if(digitalRead(BLED)){      //LED ist aus
      if(!LEDStatusCounter){
        LEDStatusCounter = LEDalivetime;
        digitalWrite(BLED, LOW);
      }
      else LEDStatusCounter--; 
    }
    else {                      //LED ist an
      if(!LEDStatusCounter){
        LEDStatusCounter = LEDalivecycle;
        digitalWrite(BLED, HIGH);
      }
      else LEDStatusCounter--;
    }
  }
  if(LEDStatusIntern == LEDBlinkOnce){
    digitalWrite(BLED, LOW);
    LEDStatus = LEDOff;  
  }
  if(LEDStatus == LEDAusVorbereitet){
    if(!LEDStatusCounter){
      LEDStatusCounter = LEDAusVorbcycle;
      if(digitalRead(BLED)) digitalWrite(BLED, LOW);
      else digitalWrite(BLED, HIGH);
    }
    else LEDStatusCounter--;
    if(!LEDPeriodCounter){
      LEDStatus = LEDAlive;
    }
    else LEDPeriodCounter--;
  }
}

void ISRSaveData(void){
  Serial.println("Interrupt to save data!");
  DataSaving();
  digitalWrite(BLED, HIGH);
  Serial.println("Data Saved");
  detachInterrupt(INT);
  ESP.deepSleep(10);
}

void LED(int LEDstatus){
  LEDStatus = LEDstatus;       // LED Status setzen dauert 1 sec.
  
  yield();
  ts.update();
  delay(10);
  yield();
  ts.update();
  delay(10);
}

void * UBat(){
  Serial.print("UBat ");
  adcwert = analogRead(A0);    //aktuellen ADC Wert lesen
  adcmittelwert = (adcmittelwert + analogRead(A0))/2;

  if(!fESPrunning){
    if(adcmittelwert > adcstartgrenzwert){    //Wert ist über dem Startwert, ESP wieder hochfahren
      fESPrunning = true;
      Serial.print("ESP funktioniert");
      LEDStatus = LEDAlive;                   //LED alive
      ts.enable(0);
      ts.enable(2);
      ts.enable(3);
    }
  }

  if(fESPrunning){
    if(adcmittelwert < adcstopgrenzwert){
      fESPrunning = false;
      Serial.print("ESP ruht");
      fDataSaving = true;
      LEDStatus = LEDOff;                   //LED aus
      ts.disable(0);
      ts.disable(2);
      ts.disable(3);
    }
  }
  
  Serial.print("Level = ");
  Serial.print(adcwert);
  Serial.print(" Mittelwert = ");
  Serial.println(adcmittelwert);
}

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBufferNTP[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (wifiUDPNTP.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpserver, ntpServerIP);
  Serial.print(ntpserver);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = wifiUDPNTP.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      wifiUDPNTP.read(packetBufferNTP, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBufferNTP[40] << 24;
      secsSince1900 |= (unsigned long)packetBufferNTP[41] << 16;
      secsSince1900 |= (unsigned long)packetBufferNTP[42] << 8;
      secsSince1900 |= (unsigned long)packetBufferNTP[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBufferNTP, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBufferNTP[0] = 0b11100011;   // LI, Version, Mode
  packetBufferNTP[1] = 0;     // Stratum, or type of clock
  packetBufferNTP[2] = 6;     // Polling Interval
  packetBufferNTP[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBufferNTP[12] = 49;
  packetBufferNTP[13] = 0x4E;
  packetBufferNTP[14] = 49;
  packetBufferNTP[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  wifiUDPNTP.beginPacket(address, 123); //NTP requests are to port 123
  wifiUDPNTP.write(packetBufferNTP, NTP_PACKET_SIZE);
  wifiUDPNTP.endPacket();
}

void printTime(time_t t)
{
  sPrintI00(hour(t));
  sPrintDigits(minute(t));
  sPrintDigits(second(t));
  Serial.print(' ');
  Serial.print(dayShortStr(weekday(t)));
  Serial.print(' ');
  sPrintI00(day(t));
  Serial.print(' ');
  Serial.print(monthShortStr(month(t)));
  Serial.print(' ');
  Serial.print(year(t));
  Serial.println(' ');
}

//Print an integer in "00" format (with leading zero).
//Input value assumed to be between 0 and 99.
void sPrintI00(int val)
{
  if (val < 10) Serial.print('0');
  Serial.print(val, DEC);
  return;
}

//Print an integer in ":00" format (with leading zero).
//Input value assumed to be between 0 and 99.
void sPrintDigits(int val)
{
  Serial.print(':');
  if (val < 10) Serial.print('0');
  Serial.print(val, DEC);
}
