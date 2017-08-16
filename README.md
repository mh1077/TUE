# TUE
Tochteruhrempfänger für MRClock

Dieser Empfänger verarbeitet die Multicast Telegramme eines MRClock Zeitzeichenservers. Aus einer LiPo Zelle (4.2 V) werden die 12 Volt bzw. 24 Volt für die Tochteruhren, auch Nebenuhren genannt, generiert und gleichzeitig der Empfänger (ESP8266) versorgt. Eine Ladeschaltung läd aus einer 5V Versorgung (USB oder Steckernetzteil) die LiPo Zelle.  

Eine Abschaltung bei niedriger LiPo Zellenspannung (ab ca. 3.1V) ist eingebaut. Der Empfänger behält die an der Tochteruhr angezeigte Uhrzeit gespeichert und läuft während dem Ladevorgang ab ca. 3.6V wieder los.

24 Stunden nach dem letzten MRClock Zeitzeichenempfang stellt der Empfänger, sofern konfiguriert, auf NTP Zeitzeichen um. Es wird die MEZ mit automatischer Sommer-/Winterzeitumstellung angezeigt. Wird der Empfänger neu gestartet, sei es durch Reset oder Einschalten, wird nach ca. 6 Minuten auf NTP umgestellt wenn kein MRClock Telegramm empfangen wurde.

Das Projekt enthält die EAGLE Dateien, den Arduino Sourcecode und eine Bedienungsanleitung für den Tochteruhrempfänger.
